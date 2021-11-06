# Linux Web Server

## Features:
1. Support concurrent connections of multiple clients
2. Generate server request statistics

## How to use:
This is a web server with multi-processing features in C. To start the server, run
```
make
./multi-server [WEBROOT DIRECTORY] [PORT NUMBER]
```

[WEBROOT DIRECTORY] is the directory that you wish the server users to have access to. For example, if the [WEBROOT DIRECTORY] is `linux-web-server`, then server users can load `multi-server.c` file.

To connect to the server, you can type `[SERVER IP ADDRESS]:[PORT NUMBER]/[FILE RELATIVE PATH]` on your browser.
To obtain the statistics of requests so far on the server, user the url `[SERVER IP ADDRESS]:[PORT NUMBER]/statistics`.
