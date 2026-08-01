#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/lib/types.cpp"
#include "src/lib/dvalue.cpp"
#include "src/lib/functionlib.cpp"
#include "src/lib/hash.cpp"
#include "src/lib/sys.cpp"
#include "src/lib/uri.cpp"

namespace task_workers {
task_queue::Result submit(const String&, const DValue&) { return(task_queue::Result{}); }
task_queue::Result status(const String&) { return(task_queue::Result{}); }
task_queue::Result await(const String&, u64) { return(task_queue::Result{}); }
task_queue::Result cancel(const String&) { return(task_queue::Result{}); }
}

static bool private_mode(String path, mode_t expected)
{
	struct stat status;
	return(stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == expected);
}

static bool complete_value(const StringMap& data)
{
	String value = data.at("value");
	if(value == "old")
		return(true);
	if(value.size() != 32768 || (value[0] != 'a' && value[0] != 'b'))
		return(false);
	return(value.find_first_not_of(value[0]) == String::npos);
}

int main()
{
	char directory_template[] = "/tmp/bearer-session-state-XXXXXX";
	char* directory = mkdtemp(directory_template);
	if(!directory)
		return(1);
	bool ok = true;
	auto need = [&](bool condition, const char* name) {
		if(!condition)
		{
			std::cerr << name << "\n";
			ok = false;
		}
	};

	ServerState server;
	server.config["SESSION_PATH"] = directory;
	server.config["SESSION_TIME"] = "1";
	Request request;
	request.server = &server;
	context = &request;

	String expired_id(64, 'e');
	String expired_path = server.config["SESSION_PATH"] + "/" + expired_id;
	int expired_fd = open(expired_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
	need(expired_fd >= 0, "create expired session");
	if(expired_fd >= 0)
	{
		write(expired_fd, "value=expired", 13);
		close(expired_fd);
		struct timespec old_time[2] = {{0, 0}, {0, 0}};
		old_time[0].tv_sec = time() - 2;
		old_time[1].tv_sec = time() - 2;
		utimensat(AT_FDCWD, expired_path.c_str(), old_time, 0);
	}

	String session_id = session_start("native-session");
	request.session["value"] = "old";
	save_session_data(session_id, request.session);
	String session_path = server.config["SESSION_PATH"] + "/" + session_id;
	need(private_mode(server.config["SESSION_PATH"], S_IRWXU), "private session directory");
	need(private_mode(session_path, S_IRUSR | S_IWUSR), "private session file");
	need(!file_exists(expired_path), "bounded expiration cleanup");

	pid_t writer = fork();
	need(writer >= 0, "fork writer");
	if(writer == 0)
	{
		for(int i = 0; i < 40; i++)
		{
			StringMap data;
			data["value"] = String(32768, i % 2 ? 'a' : 'b');
			save_session_data(session_id, data);
		}
		_exit(0);
	}
	if(writer > 0)
	{
		int status = 0;
		while(waitpid(writer, &status, WNOHANG) == 0)
			need(complete_value(load_session_data(session_id)), "atomic session read");
		need(WIFEXITED(status) && WEXITSTATUS(status) == 0, "writer status");
	}

	request.cookies["expired-session"] = expired_id;
	String replacement = session_start("expired-session");
	need(replacement != expired_id && !file_exists(expired_path), "expired cookie replacement");
	context = 0;
	std::filesystem::remove_all(directory);
	return(ok ? 0 : 1);
}
