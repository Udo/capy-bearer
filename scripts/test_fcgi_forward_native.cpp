#include "src/lib/types.cpp"
#include "src/lib/dvalue.cpp"
#include "src/lib/functionlib.cpp"
#include "src/lib/fcgi_forward.h"
#include <sys/wait.h>

static String record(unsigned char type, const String& content)
{
    String out;
    unsigned char header[8] = {1, type, 0, 1, (unsigned char)(content.size() >> 8), (unsigned char)content.size(), 0, 0};
    out.append((const char*)header, sizeof(header));
    out.append(content);
    return(out);
}

static pid_t server(const String& path, int mode)
{
    pid_t pid = fork();
    if(pid != 0)
        return(pid);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path.data(), path.size());
    unlink(path.c_str());
    if(listener < 0 || bind(listener, (sockaddr*)&address, offsetof(sockaddr_un, sun_path) + path.size() + 1) != 0 || listen(listener, 1) != 0)
        _exit(2);
    int client = accept(listener, 0, 0);
    char input[4096];
    if(client < 0 || read(client, input, sizeof(input)) <= 0)
        _exit(3);
    if(mode == 1)
    {
        String response = record(6, "Status: 201 Created\r\nContent-Type: text/plain\r\n\r\nok") + record(3, String(8, 0));
        for(char value : response)
            if(send(client, &value, 1, MSG_NOSIGNAL) != 1) _exit(4);
    }
    else if(mode == 2)
    {
        String response = record(6, "x");
        for(int i = 0; i < 100; i++)
        {
            if(send(client, response.data(), response.size(), MSG_NOSIGNAL) <= 0) break;
            usleep(20000);
        }
    }
    else
    {
        String response = record(6, String(4096, 'x')) + record(3, String(8, 0));
        send(client, response.data(), response.size(), MSG_NOSIGNAL);
    }
    close(client);
    close(listener);
    unlink(path.c_str());
    _exit(0);
}

static bool wait_socket(const String& path)
{
    for(int i = 0; i < 500; i++)
    {
        if(access(path.c_str(), F_OK) == 0) return(true);
        usleep(1000);
    }
    return(false);
}

int main()
{
    auto seconds = []() { timespec now{}; clock_gettime(CLOCK_MONOTONIC, &now); return((f64)now.tv_sec + (f64)now.tv_nsec / 1000000000.0); };
    signal(SIGPIPE, SIG_IGN);
    String path = "/tmp/bearer-fcgi-forward-" + std::to_string((long long)getpid()) + ".sock";
    bool ok = true;
    try { regex_search_all("", String(1025, 'a'), ""); ok = false; } catch(const std::runtime_error&) {}
    try { regex_split_strings("", String(1025, 'a'), ""); ok = false; } catch(const std::runtime_error&) {}
    auto run = [&](int mode, u64 limit) {
        pid_t child = server(path, mode);
        if(child <= 0 || !wait_socket(path)) { ok = false; return(FcgiForwardResult()); }
        StringMap params; params["SCRIPT_FILENAME"] = "/test.uce";
        FcgiForwardResult result = fcgi_forward_request(path, params, "", 1, limit);
        int status = 0; waitpid(child, &status, 0);
        if(!WIFEXITED(status)) ok = false;
        return(result);
    };
    FcgiForwardResult valid = run(1, 1024);
    if(!valid.ok || valid.status != 201 || valid.body != "ok") ok = false;
    f64 started = seconds();
    FcgiForwardResult deadline = run(2, 1024);
    f64 elapsed = seconds() - started;
    if(deadline.ok || elapsed < 0.8 || elapsed > 1.8) ok = false;
    FcgiForwardResult limited = run(3, 128);
    if(limited.ok || limited.error.find("output limit") == String::npos) ok = false;
    unlink(path.c_str());
    if(!ok) return(1);
    return(0);
}
