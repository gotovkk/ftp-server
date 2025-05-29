#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <sstream>
#include <fstream>
#include <filesystem> // C++17
#include <cstdlib>    
#include <cstring>   
#include <system_error>
#include <algorithm>    // <-- УБЕДИСЬ, ЧТО ЭТА СТРОКА ЗДЕСЬ
#include <cctype>       // <-- И ЭТА ТОЖЕ ЗДЕСЬ

#ifdef _WIN32
    #define _WINSOCK_DEPRECATED_NO_WARNINGS // Для старых функций вроде inet_ntoa
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib") // Линкуем библиотеку Winsock
    using socklen_t = int;
    #define CLOSE_SOCKET(s) closesocket(s)
#else 
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <cerrno>  
    #define SOCKET int
    #define INVALID_SOCKET (-1)
    #define SOCKET_ERROR (-1)
    #define CLOSE_SOCKET(s) close(s)
    #define WSAGetLastError() (errno)
#endif
// --- End Platform specific includes ---

namespace fs = std::filesystem;

const int DEFAULT_PORT = 2121; 
const int BUFFER_SIZE = 4096;
const std::string SERVER_ROOT = "ftp_root";

bool initialize_networking() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return false;
    }
#endif
    return true;
}

void cleanup_networking() {
#ifdef _WIN32
    WSACleanup();
#endif
}

void send_response(SOCKET client_socket, const std::string& response) {
    std::cout << " S -> C: " << response << std::endl;
    std::string full_response = response + "\r\n"; // FTP ответы заканчиваются \r\n
    send(client_socket, full_response.c_str(), full_response.length(), 0);
}

std::string get_ip_address(const sockaddr_in& addr) {
#ifdef _WIN32
    return inet_ntoa(addr.sin_addr);
#else
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(addr.sin_addr), ip_str, INET_ADDRSTRLEN);
    return std::string(ip_str);
#endif
}

bool create_archive(const fs::path& dir_path, const fs::path& archive_path) {
    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        std::cerr << "Error: Directory not found or not a directory: " << dir_path << std::endl;
        return false;
    }

    std::string command = "zip -qr \"" + archive_path.string() + "\" .";
    std::cout << "Executing archive command: " << command << " in directory " << dir_path << std::endl;

    fs::path old_cwd;
    try {
         old_cwd = fs::current_path();
         fs::current_path(dir_path); 
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Filesystem error changing directory: " << e.what() << std::endl;
        return false;
    }

    int result = system(command.c_str());

    try {
        fs::current_path(old_cwd);
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Filesystem error changing back directory: " << e.what() << std::endl;
    }


    if (result != 0) {
        std::cerr << "Error creating archive. Command returned: " << result << std::endl;
        std::error_code ec;
        fs::remove(archive_path, ec);
        return false;
    }
    std::cout << "Archive created successfully: " << archive_path << std::endl;
    return true;
}

// Распаковка архива (используя внешнюю утилиту)
bool extract_archive(const fs::path& archive_path, const fs::path& target_dir) {
     if (!fs::exists(archive_path) || !fs::is_regular_file(archive_path)) {
        std::cerr << "Error: Archive file not found: " << archive_path << std::endl;
        return false;
    }
    if (!fs::exists(target_dir)) {
        std::error_code ec;
        fs::create_directories(target_dir, ec);
        if (ec) {
             std::cerr << "Error creating target directory: " << target_dir << " - " << ec.message() << std::endl;
            return false;
        }
    } else if (!fs::is_directory(target_dir)) {
         std::cerr << "Error: Target path exists but is not a directory: " << target_dir << std::endl;
        return false;
    }


    std::string command = "unzip -oq \"" + archive_path.string() + "\" -d \"" + target_dir.string() + "\"";
    std::cout << "Executing extract command: " << command << std::endl;

    int result = system(command.c_str());

    if (result != 0) {
        std::cerr << "Error extracting archive. Command returned: " << result << std::endl;
        return false;
    }
    std::cout << "Archive extracted successfully to: " << target_dir << std::endl;
    return true;
}

class ClientSession {
private:
    SOCKET control_socket;
    SOCKET data_listen_socket;
    SOCKET data_socket;
    bool logged_in;
    std::string username;
    fs::path current_directory;
    fs::path server_root_path;
    bool passive_mode;
    std::string server_ip;

    fs::path resolve_client_path(const std::string& client_path_str) {
        fs::path client_path(client_path_str);
        fs::path combined_path;

        if (client_path.is_absolute()) {
            auto path_str = client_path.relative_path().string();
            if (!path_str.empty() && (path_str[0] == '/' || path_str[0] == '\\')) {
                 path_str = path_str.substr(1);
            }
             combined_path = server_root_path / path_str;
        } else {
            combined_path = server_root_path / current_directory / client_path;
        }

        fs::path canonical_path;
        std::error_code ec;
        canonical_path = fs::weakly_canonical(combined_path, ec); 
         if (ec) {
             std::cerr << "Error canonicalizing path: " << combined_path << " - " << ec.message() << std::endl;
             return fs::path();
         }


        auto root_canonical = fs::weakly_canonical(server_root_path, ec);
         if (ec) {
             std::cerr << "Error canonicalizing server root path: " << server_root_path << " - " << ec.message() << std::endl;
             return fs::path();
         }

        std::string canonical_str = canonical_path.string();
        std::string root_str = root_canonical.string();

        if (canonical_str.rfind(root_str, 0) != 0) {
             std::cerr << "Security Error: Client tried to access path outside root: " << combined_path << " (resolved to: " << canonical_path << ")" << std::endl;
             return fs::path();
         }

        return canonical_path;
    }

bool setup_pasv_socket_only() {
    if (data_listen_socket != INVALID_SOCKET) {
        CLOSE_SOCKET(data_listen_socket);
        data_listen_socket = INVALID_SOCKET;
    }

    data_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (data_listen_socket == INVALID_SOCKET) {
        perror("socket() failed for data listen");
        return false;
    }

    sockaddr_in data_addr = {};
    data_addr.sin_family = AF_INET;
    data_addr.sin_addr.s_addr = INADDR_ANY;
    data_addr.sin_port = 0; 

    if (bind(data_listen_socket, (sockaddr*)&data_addr, sizeof(data_addr)) == SOCKET_ERROR) {
        perror("bind() failed for data listen");
        CLOSE_SOCKET(data_listen_socket);
        data_listen_socket = INVALID_SOCKET;
        return false;
    }

    if (listen(data_listen_socket, 1) == SOCKET_ERROR) {
        perror("listen() failed for data listen");
        CLOSE_SOCKET(data_listen_socket);
        data_listen_socket = INVALID_SOCKET;
        return false;
    }

    if (server_ip.empty()) {
         sockaddr_in server_sock_addr;
         socklen_t server_sock_len = sizeof(server_sock_addr);
         if (getsockname(control_socket, (sockaddr*)&server_sock_addr, &server_sock_len) == SOCKET_ERROR) {
             perror("getsockname() failed for control socket in EPSV preparation");
         } else {
            server_ip = get_ip_address(server_sock_addr);
         }
    }
    return true;
}


bool setup_pasv() {
    if (!setup_pasv_socket_only()) {
        return false; 
    }

    sockaddr_in data_addr; 
    socklen_t len = sizeof(data_addr);
    if (getsockname(data_listen_socket, (sockaddr*)&data_addr, &len) == SOCKET_ERROR) {
         perror("getsockname() failed for data listen");
         CLOSE_SOCKET(data_listen_socket);
         data_listen_socket = INVALID_SOCKET;
         return false;
     }

    int port = ntohs(data_addr.sin_port);
    int p1 = port / 256;
    int p2 = port % 256;

    if (server_ip.empty()) { 
        sockaddr_in server_sock_addr;
        socklen_t server_sock_len = sizeof(server_sock_addr);
        if (getsockname(control_socket, (sockaddr*)&server_sock_addr, &server_sock_len) == SOCKET_ERROR) {
            perror("getsockname() failed for control socket");
            CLOSE_SOCKET(data_listen_socket);
            data_listen_socket = INVALID_SOCKET;
            return false;
        }
        server_ip = get_ip_address(server_sock_addr);
    }

    std::string pasv_response = "227 Entering Passive Mode (";
    std::string ip_to_send = server_ip;
    std::replace(ip_to_send.begin(), ip_to_send.end(), '.', ','); 
    pasv_response += ip_to_send + "," + std::to_string(p1) + "," + std::to_string(p2) + ")";

    send_response(control_socket, pasv_response);
    passive_mode = true;
    return true;
}

    bool accept_data_connection() {
         if (!passive_mode || data_listen_socket == INVALID_SOCKET) {
             send_response(control_socket, "425 Use PASV first.");
             return false;
         }

         std::cout << "Waiting for data connection on PASV socket..." << std::endl;
         sockaddr_in client_data_addr;
         socklen_t client_data_len = sizeof(client_data_addr);
         data_socket = accept(data_listen_socket, (sockaddr*)&client_data_addr, &client_data_len);

         CLOSE_SOCKET(data_listen_socket);
         data_listen_socket = INVALID_SOCKET;

         if (data_socket == INVALID_SOCKET) {
             perror("accept() failed for data connection");
             send_response(control_socket, "426 Connection closed; transfer aborted (Accept failed).");
             return false;
         }

         std::cout << "Data connection established from "
                   << get_ip_address(client_data_addr) << ":"
                   << ntohs(client_data_addr.sin_port) << std::endl;
         return true;
    }

    void close_data_connection() {
        if (data_socket != INVALID_SOCKET) {
            CLOSE_SOCKET(data_socket);
            data_socket = INVALID_SOCKET;
        }
        if (data_listen_socket != INVALID_SOCKET) { 
            CLOSE_SOCKET(data_listen_socket);
            data_listen_socket = INVALID_SOCKET;
        }
         passive_mode = false;
         std::cout << "Data connection closed." << std::endl;
    }

    // --- Command Handlers ---
    void handle_user(const std::string& arg) {
        if (logged_in) {
            send_response(control_socket, "230 Already logged in.");
            return;
        }
        if (arg.empty()) {
            send_response(control_socket, "501 Syntax error in parameters or arguments (Username missing).");
            return;
        }
    
        username = arg;
        logged_in = true;
        current_directory = "";
        
        send_response(control_socket, "230 User logged in, proceed. Welcome " + username + "!"); 
        std::cout << "User " << username << " logged in directly (no password required)." << std::endl;
    }

    void handle_pass(const std::string& arg) {
        if (logged_in) { 
             send_response(control_socket, "230 Already logged in.");
        } else {
             send_response(control_socket, "503 Bad sequence of commands. Use USER first."); 
        }
    }

    void handle_pwd(const std::string& arg) {
        if (!logged_in) { send_response(control_socket, "530 Not logged in."); return; }

        fs::path client_view_path = "/"; 
        client_view_path /= current_directory; 

        std::string path_str = client_view_path.generic_string();
        if (path_str.empty() || path_str == ".") path_str = "/"; 

        send_response(control_socket, "257 \"" + path_str + "\" is the current directory.");
    }

     void handle_cwd(const std::string& arg) {
         if (!logged_in) { send_response(control_socket, "530 Not logged in."); return; }

         fs::path requested_path_raw = arg;
         fs::path target_full_path;

         if (arg == "/" || arg == "\\" ) {
             target_full_path = server_root_path;
         } else {
             target_full_path = resolve_client_path(arg);
         }


         if (target_full_path.empty()) {
             send_response(control_socket, "550 Failed to change directory: Invalid path or permission denied.");
             return;
         }


         std::error_code ec;
         if (fs::exists(target_full_path, ec) && fs::is_directory(target_full_path, ec)) {
             current_directory = fs::relative(target_full_path, server_root_path, ec);
             if (ec) {
                  std::cerr << "Error getting relative path: " << ec.message() << std::endl;
                  send_response(control_socket, "550 Failed to change directory: Internal error.");
                  current_directory = "";
             } else {
                 current_directory = current_directory.lexically_normal();
                 if (current_directory == ".") current_directory = ""; 

                 std::cout << " CWD to server path: " << target_full_path
                           << ", relative client path: " << current_directory.string() << std::endl;
                 send_response(control_socket, "250 Directory successfully changed.");
             }
         } else {
             if (ec) {
                 std::cerr << "Filesystem error checking directory " << target_full_path << ": " << ec.message() << std::endl;
                 send_response(control_socket, "550 Failed to change directory: Filesystem error.");
             } else {
                 send_response(control_socket, "550 Failed to change directory: No such file or directory.");
             }
         }
     }


    void handle_list(const std::string& arg) {
         if (!logged_in) { send_response(control_socket, "530 Not logged in."); return; }
         if (!passive_mode) { send_response(control_socket, "425 Use PASV first."); return; }

         fs::path dir_to_list = resolve_client_path(""); 
         if (dir_to_list.empty()) {
            send_response(control_socket, "550 Cannot list directory: Invalid path.");
            return;
         }


         std::error_code ec_exists;
         if (!fs::exists(dir_to_list, ec_exists) || !fs::is_directory(dir_to_list, ec_exists)) {
             send_response(control_socket, "550 Cannot list directory: Not found or not a directory.");
              if(ec_exists) std::cerr << " Filesystem error on LIST: " << ec_exists.message() << std::endl;
             return;
         }

         send_response(control_socket, "150 Here comes the directory listing.");

         if (!accept_data_connection()) {
             return;
         }

         std::stringstream list_ss;
         std::error_code ec_iter;
         try {
             for (const auto& entry : fs::directory_iterator(dir_to_list, ec_iter)) {
                 list_ss << entry.path().filename().string();
                 if (entry.is_directory(ec_iter)) {
                     list_ss << "/"; 
                 }
                  list_ss << "\r\n";
                 if(ec_iter) { 
                      std::cerr << " Error iterating directory entry " << entry.path() << ": " << ec_iter.message() << std::endl;
                      ec_iter.clear();
                 }
             }
              if(ec_iter) {
                 std::cerr << " Error starting directory iteration for " << dir_to_list << ": " << ec_iter.message() << std::endl;
             }

         } catch (const fs::filesystem_error& e) {
             std::cerr << "Filesystem exception during LIST: " << e.what() << std::endl;
         }


         std::string list_data = list_ss.str();
         if (!list_data.empty()) {
            ssize_t bytes_sent = send(data_socket, list_data.c_str(), list_data.length(), 0);
             if (bytes_sent == SOCKET_ERROR) {
                 perror("send() failed for LIST data");
                 close_data_connection();
                 send_response(control_socket, "426 Connection closed; transfer aborted (Send failed).");
                 return;
             } else {
                 std::cout << "Sent " << bytes_sent << " bytes of directory listing." << std::endl;
             }
         }


         close_data_connection();
         send_response(control_socket, "226 Directory send OK.");
    }

    void handle_retr(const std::string& arg) {
        if (!logged_in) { send_response(control_socket, "530 Not logged in."); return; }
        if (!passive_mode) { send_response(control_socket, "425 Use PASV first."); return; }
        if (arg.empty()) { send_response(control_socket, "501 Syntax error in parameters or arguments (Filename missing)."); return;}

        fs::path requested_path = resolve_client_path(arg);
        if (requested_path.empty()) {
            send_response(control_socket, "550 Failed to access file or directory: Invalid path or permission denied.");
            return;
        }

        std::error_code ec;
        bool is_dir = fs::is_directory(requested_path, ec);
         if(ec) {
             std::cerr << " Filesystem error checking path " << requested_path << ": " << ec.message() << std::endl;
              send_response(control_socket, "550 Failed to access file or directory: Filesystem error.");
              return;
         }
        bool is_file = fs::is_regular_file(requested_path, ec);
         if(ec) {
              std::cerr << " Filesystem error checking path " << requested_path << ": " << ec.message() << std::endl;
               send_response(control_socket, "550 Failed to access file or directory: Filesystem error.");
               return;
         }

        if (!is_dir && !is_file) {
            send_response(control_socket, "550 Failed to retrieve file or directory: Not found or not a regular file/directory.");
            return;
        }

        fs::path file_to_send;
        bool temporary_archive = false;
        std::string filename_for_client = requested_path.filename().string();


        if (is_dir) {
            std::cout << "RETR requested for directory: " << requested_path << ". Archiving..." << std::endl;
            std::string temp_archive_name = "ftp_temp_archive_" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".zip";
            file_to_send = fs::temp_directory_path() / temp_archive_name; 
            filename_for_client += ".zip";


            if (!create_archive(requested_path, file_to_send)) {
                send_response(control_socket, "550 Failed to create archive for the directory.");
                 std::error_code rm_ec;
                 fs::remove(file_to_send, rm_ec);
                return;
            }
            temporary_archive = true;
            std::cout << "Archive created: " << file_to_send << std::endl;
        } else {
            file_to_send = requested_path;
             std::cout << "RETR requested for file: " << file_to_send << std::endl;
        }


         std::ifstream file_stream(file_to_send, std::ios::binary | std::ios::ate); 
        if (!file_stream.is_open()) {
            send_response(control_socket, "550 Failed to open file.");
             if (temporary_archive) {
                 std::error_code rm_ec;
                 fs::remove(file_to_send, rm_ec); 
             }
            return;
        }

         std::streamsize file_size = file_stream.tellg();
         file_stream.seekg(0, std::ios::beg); 

        send_response(control_socket, "150 Opening BINARY mode data connection for " + filename_for_client + " (" + std::to_string(file_size) + " bytes).");

        if (!accept_data_connection()) {
            file_stream.close();
             if (temporary_archive) {
                 std::error_code rm_ec;
                 fs::remove(file_to_send, rm_ec);
             }
            return;
        }

        char buffer[BUFFER_SIZE];
        std::streamsize bytes_sent_total = 0;
        bool transfer_ok = true;

        while (file_stream.read(buffer, sizeof(buffer)) || file_stream.gcount() > 0) {
            std::streamsize bytes_read = file_stream.gcount();
            ssize_t bytes_sent = send(data_socket, buffer, bytes_read, 0);

            if (bytes_sent == SOCKET_ERROR) {
                perror("send() failed during RETR");
                send_response(control_socket, "426 Connection closed; transfer aborted (Send failed).");
                transfer_ok = false;
                break;
            }
             if (bytes_sent < bytes_read) {
                 std::cerr << "Warning: Short send during RETR (" << bytes_sent << "/" << bytes_read << ")" << std::endl;
            }
             bytes_sent_total += bytes_sent;
        }

        file_stream.close();
        close_data_connection(); 

         if (temporary_archive) {
             std::cout << "Removing temporary archive: " << file_to_send << std::endl;
             std::error_code rm_ec;
             fs::remove(file_to_send, rm_ec);
             if (rm_ec) {
                 std::cerr << "Warning: Failed to remove temporary archive " << file_to_send << ": " << rm_ec.message() << std::endl;
             }
         }


        if (transfer_ok) {
             std::cout << "File/Archive sent successfully (" << bytes_sent_total << " bytes)." << std::endl;
            send_response(control_socket, "226 Transfer complete.");
        } else {
             std::cout << "File/Archive transfer failed." << std::endl;
         }
    }

    void handle_dele(const std::string& arg) {
        if (!logged_in) { send_response(control_socket, "530 Not logged in."); return; }
        if (arg.empty()) {
            send_response(control_socket, "501 Syntax error in parameters or arguments (Filename missing).");
            return;
        }
    
        fs::path path_to_delete = resolve_client_path(arg);
        if (path_to_delete.empty()) {
            send_response(control_socket, "550 Requested action not taken. File unavailable (e.g., file not found, no access, or invalid path).");
            return;
        }
    
        std::error_code ec;
        if (!fs::exists(path_to_delete, ec)) {
            send_response(control_socket, "550 Requested action not taken. File unavailable (e.g., file not found).");
            return;
        }
    
        if (fs::is_directory(path_to_delete, ec)) {
            send_response(control_socket, "550 Requested action not taken. Cannot delete a directory with DELE, use RMD.");
            return;
        }
    
        if (fs::remove(path_to_delete, ec)) {
            send_response(control_socket, "250 Requested file action okay, completed.");
            std::cout << "Deleted file: " << path_to_delete << std::endl;
        } else {
            std::cerr << "Error deleting file " << path_to_delete << ": " << ec.message() << std::endl;
            send_response(control_socket, "550 Requested action not taken. File unavailable or other error.");
        }
    }

    void handle_feat(const std::string& arg) {
        if (!logged_in) { 
            send_response(control_socket, "530 Please log in first.");
            return;
        }
    
        send_response(control_socket, "211-Features:");
        send(control_socket, " PASV\r\n", strlen(" PASV\r\n"), 0);
        send(control_socket, " UTF8\r\n", strlen(" UTF8\r\n"), 0);
    
        send_response(control_socket, "211 End");
    }

    void handle_stor(const std::string& arg) {
         if (!logged_in) { send_response(control_socket, "530 Not logged in."); return; }
         if (!passive_mode) { send_response(control_socket, "425 Use PASV first."); return; }
         if (arg.empty()) { send_response(control_socket, "501 Syntax error in parameters or arguments (Filename missing)."); return;}

        std::string client_filename = arg;
        bool is_archive_upload = false;
        fs::path target_name = client_filename; 

        std::string lower_arg = arg;
        std::transform(lower_arg.begin(), lower_arg.end(), lower_arg.begin(), ::tolower);
        if (lower_arg.length() > 4 && lower_arg.substr(lower_arg.length() - 4) == ".zip") {
            fs::path potential_file_path = resolve_client_path(arg);
            std::error_code ec;
            if (potential_file_path.empty() || !fs::is_regular_file(potential_file_path, ec)) {
                is_archive_upload = true;
                target_name = fs::path(client_filename).stem();
                 std::cout << "STOR identified as archive upload for extraction: " << client_filename << std::endl;
             } else {
                 std::cout << "STOR is a regular file upload (overwriting?): " << potential_file_path << std::endl;
             }
        } else {
             std::cout << "STOR is a regular file upload: " << client_filename << std::endl;
        }


        fs::path final_server_path; 
        fs::path temp_storage_path; 

        if (is_archive_upload) {
            final_server_path = resolve_client_path(target_name.string()); 
             std::string temp_archive_name = "ftp_temp_incoming_" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".zip";
             temp_storage_path = fs::temp_directory_path() / temp_archive_name;

        } else {
            final_server_path = resolve_client_path(client_filename);
            temp_storage_path = final_server_path; 
        }


        if (final_server_path.empty()) { 
            send_response(control_socket, "553 Requested action not taken. File name not allowed or path invalid.");
            return;
        }

         std::error_code ec;
         if (!is_archive_upload && fs::exists(final_server_path, ec)) {
             std::cout << "Warning: Overwriting existing file: " << final_server_path << std::endl;
         } else if (is_archive_upload && fs::exists(final_server_path, ec)) {
             if (!fs::is_directory(final_server_path, ec)) {
                  send_response(control_socket, "553 Requested action not taken. Target path exists but is not a directory.");
                  return;
             }
              std::cout << "Archive will be extracted into existing directory: " << final_server_path << std::endl;
         }

         std::ofstream file_stream(temp_storage_path, std::ios::binary | std::ios::trunc);
        if (!file_stream.is_open()) {
             std::cerr << "Error: Failed to open temporary file for writing: " << temp_storage_path << std::endl;
            send_response(control_socket, "553 Requested action not taken. Could not create file on server.");
            return;
        }

         send_response(control_socket, "150 Ok to send data.");

         if (!accept_data_connection()) {
             file_stream.close();
             std::error_code rm_ec;
             fs::remove(temp_storage_path, rm_ec);
             return;
         }

         char buffer[BUFFER_SIZE];
         ssize_t bytes_received;
         std::streamsize bytes_received_total = 0;
         bool transfer_ok = true;

         while ((bytes_received = recv(data_socket, buffer, sizeof(buffer), 0)) > 0) {
             file_stream.write(buffer, bytes_received);
             if (!file_stream.good()) {
                 std::cerr << "Error writing to file: " << temp_storage_path << std::endl;
                 send_response(control_socket, "451 Requested action aborted. Local error in processing (File write failed).");
                 transfer_ok = false;
                 break;
             }
             bytes_received_total += bytes_received;
         }

         if (bytes_received == SOCKET_ERROR) {
             perror("recv() failed during STOR");
             send_response(control_socket, "426 Connection closed; transfer aborted (Receive failed).");
             transfer_ok = false;
         }

         file_stream.close();
         close_data_connection(); 

         if (!transfer_ok) {
             std::cout << "File receive failed." << std::endl;
             std::error_code rm_ec;
             fs::remove(temp_storage_path, rm_ec);
             return;
         }

         std::cout << "File received successfully (" << bytes_received_total << " bytes) to " << temp_storage_path << std::endl;

         bool final_action_ok = true;
         if (is_archive_upload) {
             std::cout << "Attempting to extract archive " << temp_storage_path << " to " << final_server_path << std::endl;
             if (!extract_archive(temp_storage_path, final_server_path)) {
                 send_response(control_socket, "552 Requested file action aborted. Failed to extract archive.");
                 final_action_ok = false;
             }
             std::cout << "Removing temporary uploaded archive: " << temp_storage_path << std::endl;
             std::error_code rm_ec;
             fs::remove(temp_storage_path, rm_ec);
             if(rm_ec) std::cerr << "Warning: Failed to remove temporary archive " << temp_storage_path << ": " << rm_ec.message() << std::endl;

         }

         if (final_action_ok) {
             send_response(control_socket, "226 Transfer complete.");
             std::cout << "STOR command finished successfully for " << arg << std::endl;
         } else {
              std::cout << "STOR command failed after transfer for " << arg << std::endl;
         }
    }

    void handle_quit(const std::string& arg) {
        send_response(control_socket, "221 Service closing control connection.");
    }
    void handle_epsv(const std::string& arg) {
        if (!logged_in) {
            send_response(control_socket, "530 Please log in first.");
            return;
        }
    
        if (!arg.empty() && arg != "ALL" && arg != "1") { 
            send_response(control_socket, "501 Syntax error in parameters or arguments. (Only EPSV, EPSV ALL or EPSV 1 supported)");
            return;
        }
    
        if (!setup_pasv_socket_only()) { 
            send_response(control_socket, "425 Can't open data connection.");
            return;
        }
        
        sockaddr_in data_addr_check;
        socklen_t len_check = sizeof(data_addr_check);
        if (getsockname(data_listen_socket, (sockaddr*)&data_addr_check, &len_check) == SOCKET_ERROR) {
            perror("getsockname() failed for EPSV data listen");
            CLOSE_SOCKET(data_listen_socket);
            data_listen_socket = INVALID_SOCKET;
            send_response(control_socket, "425 Can't open data connection (getsockname failed).");
            return;
        }
        int port = ntohs(data_addr_check.sin_port);
    
        std::string epsv_response = "229 Entering Extended Passive Mode (|||" + std::to_string(port) + "|)";
        send_response(control_socket, epsv_response);
        passive_mode = true; 
    }

    void handle_syst(const std::string& arg) {
         if (!logged_in) { send_response(control_socket, "530 Not logged in."); return; }
 #ifdef _WIN32
         send_response(control_socket, "215 Windows_NT type."); 
 #else
         send_response(control_socket, "215 UNIX Type: L8");
 #endif
    }

     void handle_type(const std::string& arg) {
         if (!logged_in) { send_response(control_socket, "530 Not logged in."); return; }
         std::string upper_arg = arg;
         std::transform(upper_arg.begin(), upper_arg.end(), upper_arg.begin(), ::toupper);

         if (upper_arg == "I") {
             send_response(control_socket, "200 Switching to Binary mode.");
         } else if (upper_arg == "A") {
              send_response(control_socket, "200 Switching to ASCII mode.");
              std::cerr << "Warning: ASCII mode requested but not fully implemented." << std::endl;
         } else {
              send_response(control_socket, "504 Command not implemented for that parameter.");
         }
     }

    void handle_pasv(const std::string& arg) {
        if (!logged_in) { send_response(control_socket, "530 Not logged in."); return; }
        if (!setup_pasv()) {
             send_response(control_socket, "425 Can't open data connection.");
        }
    }

    void handle_unknown(const std::string& command) {
        send_response(control_socket, "500 Syntax error, command unrecognized: " + command);
    }


public:
    ClientSession(SOCKET sock, const std::string& root_dir)
        : control_socket(sock),
          data_listen_socket(INVALID_SOCKET),
          data_socket(INVALID_SOCKET),
          logged_in(false),
          passive_mode(false)
    {
         server_root_path = fs::absolute(root_dir);
         std::error_code ec;
         if (!fs::exists(server_root_path, ec)) {
             std::cout << "Server root directory doesn't exist, creating: " << server_root_path << std::endl;
             if (!fs::create_directories(server_root_path, ec) || ec) {
                  std::cerr << "FATAL: Could not create server root directory: " << server_root_path << " - " << ec.message() << std::endl;
                  throw std::runtime_error("Failed to create server root directory");
             }
         } else if (!fs::is_directory(server_root_path, ec) || ec) {
              std::cerr << "FATAL: Server root path exists but is not a directory: " << server_root_path << " - " << ec.message() << std::endl;
              throw std::runtime_error("Server root path is not a directory");
         }
         current_directory = ""; 

         std::cout << "Client session started. Server root: " << server_root_path << std::endl;
    }

    ~ClientSession() {
        close_data_connection(); 
        if (control_socket != INVALID_SOCKET) {
            CLOSE_SOCKET(control_socket);
        }
        std::cout << "Client session ended." << std::endl;
    }

    void run() {
        send_response(control_socket, "220 Basic C++ FTP Server Ready.");

        char buffer[BUFFER_SIZE];
        std::string command_line;

        while (true) {
            memset(buffer, 0, sizeof(buffer));
            ssize_t bytes_received = recv(control_socket, buffer, sizeof(buffer) - 1, 0);

            if (bytes_received <= 0) {
                if (bytes_received == 0) {
                    std::cout << "Client disconnected." << std::endl;
                } else {
                    perror("recv() failed for control connection");
                }
                break; 
            }

            command_line += std::string(buffer, bytes_received);

            size_t crlf_pos;
            while ((crlf_pos = command_line.find("\r\n")) != std::string::npos) {
                std::string single_command = command_line.substr(0, crlf_pos);
                command_line.erase(0, crlf_pos + 2); 

                std::cout << "C -> S: " << single_command << std::endl;

                // Парсим команду и аргумент
                std::string command;
                std::string argument;
                size_t space_pos = single_command.find(' ');
                if (space_pos != std::string::npos) {
                    command = single_command.substr(0, space_pos);
                    argument = single_command.substr(space_pos + 1);
                } else {
                    command = single_command;
                }

                transform(command.begin(), command.end(), command.begin(), ::toupper);

                if (command == "USER") handle_user(argument);
                else if (command == "PASS") handle_pass(argument);
                else if (command == "QUIT") { handle_quit(argument); goto end_session; } // Выход
                else if (command == "SYST") handle_syst(argument);
                else if (command == "TYPE") handle_type(argument);
                else if (command == "PWD") handle_pwd(argument);
                else if (command == "CWD") handle_cwd(argument);
                else if (command == "PASV") handle_pasv(argument);
                else if (command == "LIST") handle_list(argument);
                else if (command == "RETR") handle_retr(argument);
                else if (command == "STOR") handle_stor(argument);
                else if (command == "DELE") handle_dele(argument);
                else if (command == "FEAT") handle_feat(argument); 
                else if (command == "EPSV") handle_epsv(argument);
                else handle_unknown(command);
            }
        }

    end_session:
        return;
    }
};

void handle_client(SOCKET client_socket, const std::string& root_dir) {
    
     try {
         ClientSession session(client_socket, root_dir); 
         session.run();
     } catch (const std::exception& e) {
         std::cerr << "!!! Exception in client thread: " << e.what() << std::endl;
         CLOSE_SOCKET(client_socket);
     } catch (...) {
         std::cerr << "!!! Unknown exception in client thread." << std::endl;
         CLOSE_SOCKET(client_socket);
     }
}


int main() {
    if (!initialize_networking()) {
        return 1;
    }

    SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        perror("socket() failed");
        cleanup_networking();
        return 1;
    }

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; 
    server_addr.sin_port = htons(DEFAULT_PORT);

    if (bind(listen_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        perror("bind() failed");
        CLOSE_SOCKET(listen_socket);
        cleanup_networking();
        return 1;
    }

    if (listen(listen_socket, SOMAXCONN) == SOCKET_ERROR) {
        perror("listen() failed");
        CLOSE_SOCKET(listen_socket);
        cleanup_networking();
        return 1;
    }

     std::cout << "FTP Server listening on port " << DEFAULT_PORT << std::endl;
     std::cout << "Server root directory: " << fs::absolute(SERVER_ROOT).string() << std::endl;
     std::cout << "Using external commands for archiving (zip/unzip). Make sure they are installed and in PATH." << std::endl;

    while (true) {
        sockaddr_in client_addr = {};
        socklen_t client_addr_len = sizeof(client_addr);
        SOCKET client_socket = accept(listen_socket, (sockaddr*)&client_addr, &client_addr_len);

        if (client_socket == INVALID_SOCKET) {
            perror("accept() failed");
            continue;
        }

        std::cout << "\n--- Client connected from " << get_ip_address(client_addr) << ":" << ntohs(client_addr.sin_port) << " ---" << std::endl;
        std::thread client_thread(handle_client, client_socket, SERVER_ROOT);
        client_thread.detach(); 
    }

    CLOSE_SOCKET(listen_socket);
    cleanup_networking();
    return 0;
}
