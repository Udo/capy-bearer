/*
 * Copyright 2008, 2009 Andrey Zholos. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This file is part of the FastCGI C++ Class library (fcgicc) version 0.1,
 * available at http://althenia.net/fcgicc
 */

/*

$ ./test2

This starts a simple FastCGI server on ports 7000-7009.  It responds to HTTP
requests from a server such as lighttpd with the provided lighttpd.conf.  It
also responds with a particular transformation of standard input.

$ ./test2 -c

This acts as a client by sending multiple concurrent data requests to the
handler on ports 7000 through 7009 and validates the responses.

*/


#include <fcgicc.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <stdexcept>

#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <fastcgi.h>


static const int base_port = 7000;
static const int processes = 25;
static const int requests = 1000;

static const std::string param_rot13("ROT13");


struct Rot13 : public std::unary_function<char, char> {
    char operator() (char c) const {
        if (c >= 'a' && c <= 'm' || c >= 'A' && c <= 'M')
            return c + 13;
        if (c >= 'n' && c <= 'z' || c >= 'N' && c <= 'Z')
            return c - 13;
        return c;
    }
};


struct RandomChar {
    char operator()() const {
        return rand() % 256;
    }
};


struct Handler {
    int handle_request(FastCGIRequest& request)
    {
        static const std::string request_uri("REQUEST_URI");
        FastCGIRequest::Params::const_iterator it =
            request.params.find(request_uri);
        if (it != request.params.end()) {
            request.out.append("Content-Type: text/html\r\n\r\n"
                "<html><body><h3>FastCGI C++ Class (fcgicc) test</h3>"
                "<p>Request: ");
            request.out.append(it->second);
            request.out.append("</p></body></html>\n");
        } else {
            it = request.params.find(param_rot13);
            if (it != request.params.end())
                std::transform(it->second.begin(), it->second.end(),
                    std::back_inserter(request.out), Rot13());
        }
        return 0;
    }
};


int handle_data(FastCGIRequest& request)
{
    std::transform(request.in.begin(), request.in.end(),
        std::back_inserter(request.out), Rot13());
    request.in.clear();
    return 0;
}


void server()
{
    Handler handler;

    FastCGIServer server;
    server.request_handler(handler, &Handler::handle_request);
    server.data_handler(&handle_data);
    for (int i = 0; i < 10; i++)
        server.listen(base_port + i);
    server.process_forever();
}


struct Querier {
    int socket;

    void write(const char* data, size_t length) {
        for (;;) {
            int result = ::write(socket, data, length);
            if (result == length)
                return;
            if (result <= 0)
                throw std::runtime_error("write() failed in data client");
            data += result;
            length -= result;
        }
    }

    bool read(std::string& output) {
        char buf[4096];
        int result = ::read(socket, buf, sizeof(buf));
        if (result == -1)
            throw std::runtime_error("read() failed in data client");
        if (result == 0)
            return false;
        output.append(buf, result);
        return true;
    }

    void write_stream(int type, const std::string& stream, size_t& i) {
        size_t n = std::min(stream.size() - i, (std::string::size_type)65535);
        if (n >= 256 || n > 1 && rand() % 3 != 0)
            n = rand() % (n - 1) + 1;

        size_t m = i == stream.size() || rand() % 3 != 0 ? 0 :
            rand() % std::min(stream.size() - i, (std::string::size_type)63);
        char padding[m];
        bzero(padding, m);

        FCGI_Header header;
        bzero(&header, sizeof(header));
        header.version = FCGI_VERSION_1;
        header.type = type;
        header.contentLengthB1 = n / 256;
        header.contentLengthB0 = n % 256;
        header.paddingLength = m;

        write(reinterpret_cast<const char*>(&header), sizeof(header));
        write(stream.data() + i, n);
        write(padding, m);
        i += n;
    }

    void encode_size(std::string& params, size_t n) {
        if (n >> 7 == 0)
            params.push_back(static_cast<char>(n));
        else {
            char c[4];
            c[0] = static_cast<char>(n >> 24 | 0x80);
            c[1] = static_cast<char>(n >> 16);
            c[2] = static_cast<char>(n >> 8);
            c[3] = static_cast<char>(n);
            params.append(c, 4);
        }
    }

    void random_params(std::string& params) {
        int i = rand() % 15;
        if (i > 10)
            i = 0;
        for (; i >= 0; i--) {
            size_t m = rand() % 300, n = rand() % 700;
            encode_size(params, m + 1);
            encode_size(params, n);
            params.push_back('_');
            std::generate_n(std::back_inserter(params), m + n, RandomChar());
        }
    }

    void process() {
        for (int i = 0; i < requests; i++) {
            // Generate random request and send it either as a special parameter
            // or as the standard input stream. Pad with random parameters.
            std::string params, in, request;

            std::generate_n(std::back_inserter(request),
                rand() % 10000, RandomChar());

            random_params(params);
            if (rand() % 3 == 0)
                in.append(request);
            else {
                encode_size(params, param_rot13.size());
                encode_size(params, request.size());
                params.append(param_rot13);
                params.append(request);
            }
            random_params(params);

            // Connect to the server
            socket = ::socket(PF_INET, SOCK_STREAM, 0);
            if (socket == -1)
                throw std::runtime_error("socket() failed in data client");

            struct sockaddr_in sa;
            bzero(&sa, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(base_port + rand() % 10);
            sa.sin_addr.s_addr = htonl(0x7f000001);
            if (::connect(socket, (struct sockaddr*)&sa, sizeof(sa)) == -1)
                throw std::runtime_error("connect() failed in data client");

            static FCGI_BeginRequestRecord begin;
            bzero(&begin, sizeof(begin));
            begin.header.version = FCGI_VERSION_1;
            begin.header.type = FCGI_BEGIN_REQUEST;
            begin.header.contentLengthB0 = sizeof(begin.body);
            begin.body.roleB0 = FCGI_RESPONDER;
            if (rand() % 3 == 0)
                begin.body.flags = FCGI_KEEP_CONN;
            write(reinterpret_cast<const char*>(&begin), sizeof(begin));

            // Send streams in random chunks
            size_t in_i = 0, params_i = 0;
            while (in_i < in.size() || params_i < params.size()) {
                if (in_i == in.size() ||
                        params_i < params.size() && rand() % 3 == 0)
                    write_stream(FCGI_PARAMS, params, params_i);
                else
                    write_stream(FCGI_STDIN, in, in_i);
            }
            if (rand() % 3 == 0) {
                write_stream(FCGI_PARAMS, params, params_i);
                write_stream(FCGI_STDIN, in, in_i);
            } else {
                write_stream(FCGI_STDIN, in, in_i);
                write_stream(FCGI_PARAMS, params, params_i);
            }

            // Sometimes close our end of the socket
            if (rand() % 5 == 0)
                if (::shutdown(socket, SHUT_WR) == -1)
                    throw std::runtime_error("shutdown() failed "
                        "in data client");

            // Receive and verify results
            std::string output;
            std::string out, err;
            bool closed_out = false, closed_err = false;
            while (read(output)) {
            another_record:
                if (output.size() < sizeof(FCGI_Header))
                    continue;
                const FCGI_Header& header =
                    *reinterpret_cast<const FCGI_Header*>(output.data());

                if (header.version != FCGI_VERSION_1)
                    throw std::runtime_error("received: incorrect version");

                size_t content = (header.contentLengthB1 << 8) +
                    header.contentLengthB0;
                size_t padding = header.paddingLength;
                if (output.size() - sizeof(FCGI_Header) < content + padding)
                    continue;

                switch (header.type) {
                case FCGI_STDOUT:
                    if (closed_out)
                        throw std::runtime_error("received: "
                            "data on closed stream");
                    out.append(output.data() + sizeof(FCGI_Header), content);
                    if (content == 0)
                        closed_out = true;
                    break;
                case FCGI_STDERR:
                    if (closed_err)
                        throw std::runtime_error("received: "
                            "data on closed stream");
                    err.append(output.data() + sizeof(FCGI_Header), content);
                    if (content == 0)
                        closed_err = true;
                    break;
                case FCGI_END_REQUEST: {
                    if (!closed_out || !closed_err)
                        throw std::runtime_error("received: "
                            "streams not closed");
                    if (output.size() - sizeof(FCGI_Header) <
                            sizeof(FCGI_EndRequestBody))
                        continue;
                    const FCGI_EndRequestBody& body =
                        *reinterpret_cast<const FCGI_EndRequestBody*>(
                            output.data() + sizeof(FCGI_Header));
                    if ((body.appStatusB3 | body.appStatusB2 |
                        body.appStatusB1 | body.appStatusB0) != 0)
                        throw std::runtime_error("received: bad exit code");
                    if (body.protocolStatus != FCGI_REQUEST_COMPLETE)
                        throw std::runtime_error("received: bad status");
                    if (content + padding !=
                            output.size() - sizeof(FCGI_Header))
                        throw std::runtime_error("received: extra data");
                    goto request_complete;
                }
                default:
                    throw std::runtime_error("received: unexpected type");
                }

                output.erase(0, sizeof(FCGI_Header) + content + padding);
                goto another_record;
            }
            throw std::runtime_error("received: not enough data");

        request_complete:
            if (::close(socket) == -1 && errno != ECONNRESET)
                throw std::runtime_error("close() failed in data client");

            if (!err.empty())
                throw std::runtime_error("received: incorrect stderr");
            std::string result;
            std::transform(out.begin(), out.end(),
                std::back_inserter(result), Rot13());
            if (result != request)
                throw std::runtime_error("received: incorrect stdout");
        }
    }
};


void client()
{
    for (int i = 0; i < processes; i++) {
        switch (::fork()) {
        case -1:
            throw std::runtime_error("fork() failed");
        case 0:
            ::srand(i);
            Querier querier;
            querier.process();
            return;
        }
    }

    for (int i = 0; i < processes; i++) {
        int status;
        if (::wait(&status) == -1)
            throw std::runtime_error("wait() failed");
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            throw std::runtime_error("data client failed");
    }
    std::cout << "Success!\n";
}


int main(int argc, const char* argv[])
{
    try {
        static const std::string arg_client("-c");
        for (int i = 1; i < argc; i++)
            if (argv[i] == arg_client) {
                client();
                return 0;
            }

        server();
        return 0;

    } catch (std::exception& e) {
        std::cerr << "Error: " << e.what() << ".\n";
    } catch (...) {
        std::cerr << "Error.\n";
    }
    return 1;
}
