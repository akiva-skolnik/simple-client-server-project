import socket
import struct
import os
from enum import Enum
import traceback


class OpCode(Enum):
    SAVE_FILE = 100
    RETRIEVE_FILE = 200
    DELETE_FILE = 201
    LIST_FILES = 202


class ResponseStatus(Enum):
    # Success:
    FILE_RETRIEVED = 210
    FILE_LIST_RETRIEVED = 211
    SUCCESS = 212  # https://opal.openu.ac.il/mod/ouilforum/discuss.php?d=3150868&p=7479767#p7479767

    # Errors:
    NO_FILE = 1001
    NO_USER_FILES = 1002
    SERVER_ERROR = 1003


class FieldSize(Enum):  # in bytes
    # header sizes
    USER_ID = 4
    VERSION = 1
    OP = 1
    NAME_LEN = 2

    # payload sizes
    FILE_SIZE = 4
    STATUS = 2


def read_server_info() -> tuple:
    """Reads the server info from the server.info file and returns a tuple of (server_ip, server_port)"""
    with open("server.info", "r") as server_info_file:
        server_info = server_info_file.readline().strip()
    server_ip, server_port = server_info.split(':')
    return server_ip, int(server_port)


def get_local_filenames() -> list:
    """Reads the backup.info file and returns a list of the local filenames"""
    with open("backup.info", "r") as backup_info_file:
        return [line.strip() for line in backup_info_file.readlines()]


def get_file_bytes(filename: str) -> bytes:
    """Reads the file with the given filename and returns its bytes"""
    with open(filename, "rb") as file:
        return file.read()


def create_unique_user_id() -> bytes:
    """Creates a unique user ID (4 bytes)"""
    return os.urandom(FieldSize.USER_ID.value)


def send_request(client_socket: socket.socket, user_id: bytes, version: int, op_code: OpCode, filename: str = "") -> None:
    """Sends a request to the server with the given parameters
    If the request is a SAVE_FILE request, the filename parameter must be given
    """
    if not filename:
        package = struct.pack("<IBB", int.from_bytes(user_id, byteorder='big'), version, op_code.value)
    else:
        # Format: User_ID (4 bytes), Version (1 byte), OpCode (1 byte), name_len (2 bytes)
        # < means little endian, `I` means unsigned int, B means unsigned char, H means unsigned short
        package = struct.pack("<IBBH", int.from_bytes(user_id, byteorder='big'), version, op_code.value, len(filename))
        package += filename.encode()

    if op_code == OpCode.SAVE_FILE:
        file_content = get_file_bytes(filename)
        package += struct.pack("<I", len(file_content))
        package += file_content  # it's already bytes, so no need to pack it
    client_socket.send(package)


def receive_status(client_socket: socket.socket) -> int:
    """Receives the version and status code from the server and returns the status"""
    header = client_socket.recv(FieldSize.VERSION.value + FieldSize.STATUS.value)
    version, status = struct.unpack("<BH", header)  # Format: B = unsigned char (1 byte), H = unsigned short (2 bytes)
    return status


def receive_filename(client_socket: socket.socket) -> str:
    """Receives the filename from the server and returns it"""
    name_len = struct.unpack("<H", client_socket.recv(FieldSize.NAME_LEN.value))[0]  # Format: Name Length (2 bytes)
    filename = client_socket.recv(name_len).decode()
    return filename


def receive_file(client_socket: socket.socket, filename: str) -> None:
    """Receives a file (one chunk at a time) from the socket and saves it to the given filename"""
    file_size = struct.unpack("<I", client_socket.recv(FieldSize.FILE_SIZE.value))[0]
    buffer_size = 2 ** 12
    with open(filename, "wb") as file:
        # Use a loop to receive and write the file in chunks
        while file_size > 0:
            chunk = client_socket.recv(buffer_size)
            if not chunk:
                break
            file.write(chunk)
            file_size -= len(chunk)


def request_file_list(client_socket: socket.socket, user_id: bytes, version: int):
    """Requests the list of files from the server and prints it to the console"""
    send_request(client_socket, user_id, version, OpCode.LIST_FILES)
    status = receive_status(client_socket)
    if status == ResponseStatus.FILE_LIST_RETRIEVED.value:
        filename = receive_filename(client_socket)
        receive_file(client_socket, filename)
        with open(filename, "rb") as file:
            file_content = file.read()
        print("List of files on server:")
        print(file_content.decode("utf-8"))
    elif status == ResponseStatus.NO_USER_FILES.value:
        print("No files on server")
    else:
        print("Wrong status code")


def request_file_save(client_socket: socket.socket, user_id: bytes, version: int, filename: str):
    """Sends a SAVE_FILE request to the server with the given filename, and prints the result to the console"""
    send_request(client_socket, user_id, version, OpCode.SAVE_FILE, filename)
    status = receive_status(client_socket)
    if status == ResponseStatus.SUCCESS.value:
        filename = receive_filename(client_socket)
        print(f"Saved {filename} to server")
    elif status == ResponseStatus.NO_FILE.value:
        print(f"No such file {filename} on client")
    else:
        print("Wrong status code")


def request_file_retrieve(client_socket: socket.socket, user_id: bytes, version: int, filename: str):
    """Sends a RETRIEVE_FILE request to the server with the given filename, and prints the result to the console"""
    send_request(client_socket, user_id, version, OpCode.RETRIEVE_FILE, filename)
    status = receive_status(client_socket)
    if status == ResponseStatus.FILE_RETRIEVED.value:
        filename = receive_filename(client_socket)
        receive_file(client_socket, "tmp")
        print(f"Retrieved {filename} from server")
    elif status == ResponseStatus.NO_FILE.value:
        print("No such file on server")
    else:
        print("Wrong status code")


def request_file_delete(client_socket: socket.socket, user_id: bytes, version: int, filename: str):
    """Sends a DELETE_FILE request to the server with the given filename, and prints the result to the console"""
    send_request(client_socket, user_id, version, OpCode.DELETE_FILE, filename)
    status = receive_status(client_socket)
    if status == ResponseStatus.SUCCESS.value:
        filename = receive_filename(client_socket)
        print(f"Deleted {filename} from server")
    elif status == ResponseStatus.NO_FILE.value:
        print("No such file on server")
    else:
        print("Wrong status code")


def request(request_function: callable, server_ip: str, server_port: int, user_id: bytes, *args):
    """Connects to the server and calls the given request function with the given arguments"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as client_socket:
        try:
            client_socket.connect((server_ip, server_port))
            request_function(client_socket, user_id, *args)
        except Exception:
            print(request_function.__name__ + " failed:")
            traceback.print_exc()


def main() -> None:
    """Main function"""
    version = 1
    server_ip, server_port = read_server_info()
    user_id = create_unique_user_id()
    local_filenames = get_local_filenames()

    try:
        request(request_file_list, server_ip, server_port, user_id, version)

        # Save the first file to the backup
        request(request_file_save, server_ip, server_port, user_id, version, local_filenames[0])

        # Save the second file to the backup
        request(request_file_save, server_ip, server_port, user_id, version, local_filenames[1])

        # List files again
        request(request_file_list, server_ip, server_port, user_id, version)

        # Retrieve and save the first file
        request(request_file_retrieve, server_ip, server_port, user_id, version, local_filenames[0])

        # Delete the first file
        request(request_file_delete, server_ip, server_port, user_id, version, local_filenames[0])

        # Try to retrieve the deleted file (should fail)
        request(request_file_retrieve, server_ip, server_port, user_id, version, local_filenames[0])

    except Exception:
        traceback.print_exc()


if __name__ == "__main__":
    main()
