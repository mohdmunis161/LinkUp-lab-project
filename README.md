# LinkUp

**LinkUp** is a comprehensive C++ Chat & File Transfer Application that integrates advanced Computer Science concepts, including Theory of Computation (TOC) and Database Systems.

## Features

- **Real-time Chat**: Multi-user chat functionality with instant message delivery.
- **File Transfer**: Secure file transfer capabilities between users.
- **MySQL Integration**: Persistent storage for users, messages, and file metadata using a custom C++ wrapper.
- **Smart Command Router**: Uses Deterministic Finite Automata (DFA) and Pushdown Automata (PDA) for parsing and validating chat commands.
- **Web Frontend**: A modern, responsive HTML/JS frontend with Dark Mode support.
- **REST API**: C++ backend serving a RESTful API for the frontend.

## Prerequisites

To build and run this project, you need the following installed on your system:

- **C++ Compiler**: `g++` (supporting C++17)
- **OpenSSL**: `libssl-dev`
- **MySQL Client Library**: `libmysqlclient-dev`
- **Make**: Build automation tool

## Build Instructions

1.  Clone the repository (if applicable).
2.  Navigate to the project directory.
3.  Run `make` to build the application.

```bash
make
```

This will generate the `linkup_main` executable.

## Usage

1.  **Start the Server**:
    ```bash
    ./linkup_main
    ```
    The server will start and listen on the configured port (default: 8080).

2.  **Access the Frontend**:
    Open `frontend.html` in your web browser.

3.  **Login/Signup**:
    Create a new account or log in with an existing one to start chatting.

## Project Structure

- `linkup_main.cpp`: Main entry point and server logic.
- `linkup_channels.cpp/h`: Channel management.
- `linkup_chat.cpp/h`: Chat logic.
- `linkup_file_transfer.cpp/h`: File transfer logic.
- `db_utils.cpp/h`: Database utility wrapper.
- `toc.h`: Theory of Computation concepts implementation.
- `frontend.html`: Web interface.
- `db_schema.sql`: Database schema definition.


