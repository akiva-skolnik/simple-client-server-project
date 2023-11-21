#include <iostream>
#include <thread>
#include <vector>
#include <cstring>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>
#include <stdexcept>
#include <filesystem>
#include <boost/asio.hpp>

enum class OpCode : uint8_t {
    SAVE_FILE = 100,
    RETRIEVE_FILE = 200,
    DELETE_FILE = 201,
    LIST_FILES = 202
};

enum class ResponseStatus : uint16_t {
    FILE_RETRIEVED = 210,
    FILE_LIST_RETRIEVED = 211,
    SUCCESS = 212,

    NO_FILE = 1001,
    NO_USER_FILES = 1002,
    SERVER_ERROR = 1003
};

enum class FieldSize : size_t {
    USER_ID = 4,
    VERSION = 1,
    OP = 1,
    NAME_LEN = 2,
    FILE_SIZE = 4,
    STATUS = 2
};

const std::string FILE_DIR = "backupsvr/";

class RequestHandler {
public:
    RequestHandler(boost::asio::ip::tcp::socket socket) : socket(std::move(socket)) {}

    void start() {
        try {
            boost::asio::streambuf header_buffer;
            boost::asio::read(socket, header_buffer, boost::asio::transfer_exactly(static_cast<int>(FieldSize::USER_ID) + static_cast<int>(FieldSize::VERSION) + static_cast<int>(FieldSize::OP)));
            std::istream header_stream(&header_buffer);
            uint32_t user_id;
            uint8_t version
            uint8_t op;
            header_stream.read(reinterpret_cast<char*>(&user_id), sizeof(user_id));
            header_stream.read(reinterpret_cast<char*>(&version), sizeof(version));           
            header_stream.read(reinterpret_cast<char*>(&op), sizeof(op));
            
            
            if (op == static_cast<uint8_t>(OpCode::LIST_FILES)) {
                handle_list_files_request(user_id, version);
            }
            else {
                boost::asio::read(socket, header_buffer, boost::asio::transfer_exactly(static_cast<int>(FieldSize::NAME_LEN)));

                uint16_t name_len;
                header_stream.read(reinterpret_cast<char*>(&name_len), sizeof(name_len));

                std::string filename;
                filename.resize(name_len);
                boost::asio::read(socket, boost::asio::buffer(&filename[0], name_len));

                if (op == static_cast<uint8_t>(OpCode::SAVE_FILE)) {
                    handle_save_request(user_id, version, filename);
                }
                else if (op == static_cast<uint8_t>(OpCode::RETRIEVE_FILE)) {
                    handle_retrieve_request(user_id, version, filename);
                }
                else if (op == static_cast<uint8_t>(OpCode::DELETE_FILE)) {
                    handle_delete_request(user_id, version, filename);
                }
                else { // Invalid operation
                    send_response(version, ResponseStatus::SERVER_ERROR);
                }
            }
            socket.close();
        }
        catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            send_response(version, ResponseStatus::SERVER_ERROR);
        }
        
    }

private:
    boost::asio::ip::tcp::socket socket;

    void send_response(uint8_t version, ResponseStatus status, const std::string& filename = "", const std::vector<uint8_t>& file_data = {}) {
        // Send the response to the client with the following format:
        // [version (1 byte)] [status (2 bytes)] [filename length (2 bytes)] [filename (variable length)] [file size (4 bytes)] [file data (variable length)]
        // The file size and file data fields are only sent on success, and the filename length and filename fields are only sent if a filename is provided
        uint16_t name_len = static_cast<uint16_t>(filename.length());
        std::vector<uint8_t> response(sizeof(version) + sizeof(status) + (filename == "" ? 0 : sizeof(name_len) + name_len));
        
        std::memcpy(response.data(), &version, sizeof(version));
        std::memcpy(response.data() + sizeof(version), &status, sizeof(status));
        if (filename != "") {
            std::memcpy(response.data() + sizeof(version) + sizeof(status), &name_len, sizeof(name_len));
            std::memcpy(response.data() + sizeof(version) + sizeof(status) + sizeof(name_len), filename.data(), name_len);
        }
        if (!file_data.empty()) {
            uint32_t file_size = static_cast<uint32_t>(file_data.size());
            response.resize(response.size() + sizeof(file_size));
            std::memcpy(response.data() + sizeof(version) + sizeof(status) + sizeof(name_len) + name_len, &file_size, sizeof(file_size));
            response.insert(response.end(), file_data.begin(), file_data.end());
        }
        try {
            boost::asio::write(socket, boost::asio::buffer(response));
        }
        catch (std::exception& e) {
            std::cerr << "Error in handling request: " << e.what() << std::endl;
        }
    }

    void handle_save_request(uint32_t user_id, uint8_t version, const std::string& filename) {
        // Save the file to the user's directory backup
        try {
            uint32_t size;
            boost::asio::read(socket, boost::asio::buffer(&size, sizeof(size)));
            std::string user_dir = FILE_DIR + std::to_string(user_id);
            std::string file_path = user_dir + "/" + filename;
            if (size <= 0 or not validate_paths(user_dir, filename)) {
                send_response(version, ResponseStatus::SERVER_ERROR);
            }
            
            const size_t buffer_size = 4096;
            std::vector<uint8_t> buffer(buffer_size);

            // Open the file for writing in binary mode
            std::ofstream file(file_path, std::ios::binary);

            size_t data_received = 0;
            while (data_received < size) {
                size_t recv_size = socket.read_some(boost::asio::buffer(buffer));

                if (recv_size == 0)
                    break;
                file.write(reinterpret_cast<const char*>(buffer.data()), recv_size);

                data_received += recv_size;
            }

            // Close the file when done
            file.close();

            send_response(version, ResponseStatus::SUCCESS, filename);
        }
        catch (const std::exception& e) {
            send_response(version, ResponseStatus::SERVER_ERROR);
        }
    }


    void handle_retrieve_request(uint32_t user_id, uint8_t version, const std::string& filename) {
        // Retrieve the file from the user's directory backup (if it exists)
        try {
            std::string user_dir = FILE_DIR + std::to_string(user_id);

            if (validate_paths(FILE_DIR + std::to_string(user_id), filename)) {
                std::ifstream file(user_dir + "/" + filename, std::ios::binary);

                if (file.is_open()) {
                    std::vector<uint8_t> file_data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                    file.close();
                    send_response(version, ResponseStatus::FILE_RETRIEVED, filename, file_data);
                    return;
                }
            }
            send_response(version, ResponseStatus::NO_FILE);
        }
        catch (const std::exception& e) {
            std::cerr << "handle_retrieve_request error: " << e.what() << std::endl;
            send_response(version, ResponseStatus::SERVER_ERROR);
        }
    }

    void handle_delete_request(uint32_t user_id, uint8_t version, const std::string& filename) {
        // Delete the file from the user's directory backup
        try {
            std::string user_dir = FILE_DIR + std::to_string(user_id);

            if (validate_paths(user_dir, filename)) {

                if (std::remove((user_dir + "/" + filename).c_str()) == 0) {
                    send_response(version, ResponseStatus::FILE_RETRIEVED, filename);
                    return;
                }
            }
            send_response(version, ResponseStatus::NO_FILE, filename);
            
        }
        catch (const std::exception& e) {
            send_response(version, ResponseStatus::SERVER_ERROR, filename);
        }
    }

    void handle_list_files_request(uint32_t user_id, uint8_t version) {
        // List the files in the user's directory backup, by creating a list file containing the filenames
        try {
            std::string user_files_dir = FILE_DIR + std::to_string(user_id);

            if (validate_paths(user_files_dir)) {
                std::vector<std::string> user_files;

                for (const auto& entry : std::filesystem::directory_iterator(user_files_dir)) {
                    user_files.push_back(entry.path().filename().string());
                }

                if (!user_files.empty()) {
                    std::string list_filename = generate_random_filename();
                    std::string list_filepath = user_files_dir + "/" + list_filename;

                    std::ofstream list_file(list_filepath);
                    for (const std::string& file : user_files) {
                        list_file << file << "\n";
                    }
                    list_file.close();
                    std::ifstream list_file_content(list_filepath, std::ios::binary);

                    if (list_file_content.is_open()) {
                        std::vector<uint8_t> file_data((std::istreambuf_iterator<char>(list_file_content)), std::istreambuf_iterator<char>());
                        list_file_content.close();
                        send_response(version, ResponseStatus::FILE_LIST_RETRIEVED, list_filename, file_data);
                        return;
                    }
                }
            }
            send_response(version, ResponseStatus::NO_USER_FILES);
        }
        catch (const std::exception& e) {
            std::cerr << "handle_list_files_request error: " << e.what() << std::endl;
            send_response(version, ResponseStatus::SERVER_ERROR);
        }
    }

    static std::string generate_random_filename() {
        // Generate a random filename of length 32
        const std::string characters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
        const int length = 32;  
        std::string filename;
        std::srand(static_cast<unsigned>(std::time(nullptr))); // Seed the random number generator
        for (int i = 0; i < length; ++i) {
            filename += characters[rand() % characters.length()];
        }

        return filename;
    }

    bool validate_paths(const std::string& user_dir, const std::string& filename = "") {
        // Check if the paths contain any ".." to prevent directory traversal attacks
        if (user_dir.find("..") != std::string::npos || filename.find("..") != std::string::npos) {
            return false;  // Invalid paths containing ".."
        }

        // create dir if it doesn't exist
        if (!std::filesystem::exists(user_dir)) {
            if (!std::filesystem::create_directories(user_dir)) {
                std::cout << "Failed to create user directory" << std::endl;
                return false;  // Failed to create user directory
            }
        }
        return true;  // Paths are valid and exist
    }
};

class FileServer {
public:
    FileServer(const std::string& host, int port) : io_context(), acceptor(io_context), host(host), port(port) {
        boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::make_address(host), port);
        acceptor.open(endpoint.protocol());
        acceptor.bind(endpoint);
        acceptor.listen();

        std::cout << "Server is listening on " << host << ":" << port << "..." << std::endl;

        start_accept();
    }

    void start() {
        io_context.run();
    }

private:
    boost::asio::io_context io_context;
    boost::asio::ip::tcp::acceptor acceptor;
    std::string host;
    int port;

    void start_accept() {
        auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io_context);
        acceptor.async_accept(*socket, [this, socket](const boost::system::error_code& ec) {
            if (!ec) {
                std::cout << "Accepted connection from " << socket->remote_endpoint().address().to_string() << ":" << socket->remote_endpoint().port() << std::endl;
                std::thread([this, socket]() {
                    RequestHandler handler(std::move(*socket));
                    handler.start();
                }).detach();
            } else {
                std::cout << "Error accepting connection: " << ec.message() << std::endl;
            }
            start_accept();
        });
    }
};


int main() {
    // Create the FILE_DIR directory if it doesn't exist
    if (!std::filesystem::exists(FILE_DIR)) {
        if (!std::filesystem::create_directories(FILE_DIR)) {
            std::cerr << "Failed to create FILE_DIR directory" << std::endl;
            return 1;  // Return an error code
        }
    }

    FileServer file_server("127.0.0.1", 1234);
    file_server.start();

    return 0;
}