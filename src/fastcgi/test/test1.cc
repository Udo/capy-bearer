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


#include <fcgicc.h>

#include <algorithm>
#include <functional>


int handle_request(FastCGIRequest& request) {
    // This is always the first event to occur.  It occurs when the
    // server receives all parameters.  There may be more data coming on the
    // standard input stream.

    if (request.params.count("REQUEST_URI"))
        return 0;  // OK, continue processing
    else
        return 1;  // Stop processing and return error code
}

int handle_data(FastCGIRequest& request) {
    // This event occurs when data is received on the standard input stream.
    // A simple string is used to hold the input stream, so it is the
    // responsibility of the application to remember which data it has
    // processed.  The application may modify it;  new data will be appended
    // to it by the server.  The same goes for the output and error streams:
    // the application should append data to them;  the server will remove
    // all sent data from them.

    std::transform(request.in.begin(), request.in.end(),
        std::back_inserter(request.err),
        std::bind1st(std::plus<char>(), 1));
    request.in.clear();  // don't process it again
    return 0;  // still OK
}

class Application {
public:
    int handle_complete(FastCGIRequest& request) {
        // The event handler can also be a class member function.  This
        // event occurs when the parameters and standard input streams are
        // both closed, and thus the request is complete.

        request.out.append("Content-Type: text/plain\r\n\r\n");
        request.out.append("You requested: ");
        request.out.append(request.params[std::string("REQUEST_URI")]);
        return 0;
    }
};

int main() {
    Application application;

    FastCGIServer server;  // Instantiate a server

    // Set up our request handlers
    server.request_handler(&handle_request);
    server.data_handler(&handle_data);
    server.complete_handler(application, &Application::handle_complete);

    server.listen(7000);        // Listen on a TCP port
    server.listen(7001);        // ... or on two
    server.listen("./socket");  // ... and also on a local doman socket

    server.process(100);  // Process some data, but don't wait more
                            // than 100 ms for it to arrive.
    server.process();  // Process some data with no timeout
    server.process_forever();  // Process everything

    return 0;
}
