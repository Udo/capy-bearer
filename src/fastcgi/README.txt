FastCGI C++ Class library (fcgicc)

1. Introduction

This is a simple C++ class library that provides FastCGI server functionality.
FastCGI is a protocol for connecting web servers with programs that generate
content.  The protocol is described in more detail at http://www.fastcgi.com.
This library provides a single class which handles FastCGI connections on TCP/IP
or local domain sockets.  Multiple connections are handled in a single thread
using select().  When a request is ready, it is passed to an application-
supplied callback for processing, after which the generated response is sent
back to the client.


2. Version information

This is the first release of fcgicc, version 0.1.2.  It provides a full
implementation of the responder mode of FastCGI, but has undergone only limited
testing.  Use with care!


3. Licensing

fcgicc is free software, available under a BSD-style license. There is no
warranty; not even for merchantability or fitness for a particular purpose. See
the file LICENSE.txt for complete information.


4. Installing

This library depends on the FastCGI Development Kit from http://www.fastcgi.com.
For convenience the required part of it is included in this distribution and
will be used if the development kit is not found on your system.

To build and install fcgicc as a standalone library you will need CMake, which
is available from http://cmake.org.

    cd fcgicc

    cmake .
    make install

    - or -

    cmake -DPREFIX=$HOME/local .
    make install

Alternatively, it may be simpler to import the two source files into your
project and build them as part of it.


5. Using

Here is how it works:

  Client ------------> Web server ------> FastCGIServer ----------------.
          HTTP request             params                FastCGIRequest |
                                       in                               '
                                                                   Application
                                                                        .
                                                                        |
  Client <------------ Web server <------ FastCGIServer <---------------'
         HTTP response            out                   FastCGIRequest
                                  err

The web server, which is a client to the FastCGI server, forwards a request as a
set of key-value parameter pairs and a standard input stream.  The parameter
pairs are the environment variables from plain CGI, and they include such
important variables as REQUEST_URI.  The standard input stream contains data
from POST requests.

An instance of the FastCGIServer class listens for requests from the web server,
builds a FastCGIRequest instance for each one, calls event handlers defined by
the application to process them, and responds to the web server.

The application processes requests using event handlers like this:

    ...

    #include <fcgicc.h>

    ...

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

        std::transform(request.stdin.begin(), request.stdin.end(),
            std::back_inserter(request.stderr),
            std::bind1st(std::plus<char>(), 1));
        request.stdin.clear();  // don't process it again
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

    ...

The application sets up the FastCGI server like this:

    ...

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

    ...


6. Updates and feedback

This library is hosted at http://althenia.net/fcgicc. It is programmed by
Andrey Zholos <aaz@althenia.net>.  Comments, bug reports and testing results are
welcome and will be appreciated.
