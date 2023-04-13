/* ToreroServe: A Lean Web Server
 * COMP 375 - Project 02
 *
 * This program should take two arguments:
 *  1. The port number on which to bind and listen for connections
 *  2. The directory out of which to serve files.
 *
 *
 * Author 1: Tyler Kreider tkreider@sandiego.edu
 * Author 2: Jacob Weil jweil@sandiego.edu
 */

// standard C libraries
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

// operating system specific libraries
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
// C++ standard libraries
#include <vector>
#include <thread>
#include <string>
#include <iostream>
#include <system_error>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <pthread.h>
#include <regex>
#include <mutex>
#include <condition_variable>
#include <queue>


#define BUFFER_SIZE 2048
#define TRANSACTION_CLOSE 2 



using std::ios;
using std::cout;
using std::string;
using std::vector;
using std::thread;

static const int BUFFER_CAPACITY = 10;
static const int NUMTHREADS = 4;

namespace fs = std::filesystem;

int createSocketAndListen(const int port_num);
void acceptConnections(const int server_sock);
void handleClient(const int client_sock);
void sendData(int socked_fd, const char *data, size_t data_length);
int receiveData(int socked_fd, char *dest, size_t buff_size);
void badRequest(int client_sock);
void DNE(int client_sock);
void header(string file_name, const int client_sock );
void content(string file_name, const int client_sock);
string int_to_str(int x);
bool okRequest(string request); 
void sendOK(const int client_sock);
void showDirectory(int client_socket, string dir_name);
void directoryHelper(int client_socket, string dir_name);


class BoundedBuffer {
	public:
		// public constructor
		BoundedBuffer(int max_size);

		// public member functions (a.k.a. methods)
		int getItem();
		void putItem(int new_item);
		//begin section containing private (i.e. hidden) parts of the class
	private:
		int capacity;
		std::queue<int> buffer;
		std::mutex shared_mutex;
		std::condition_variable data_available;
		std::condition_variable space_available;

};
BoundedBuffer::BoundedBuffer(int max_size) {
	capacity = max_size;

	// buffer field implicitly has its default (no-arg) constructor called.
	// This means we have a new buffer with no items in it.
}

/**
 * Gets the first item from the buffer then removes it.
 */
int BoundedBuffer::getItem() {

	std::unique_lock<std::mutex> consumer_lock(this->shared_mutex);


	this->data_available.wait(consumer_lock);
	int item = this->buffer.front(); // "this" refers to the calling object...
	buffer.pop(); // ... but like Java it is optional (no this in front of buffer on this line)
	this->space_available.notify_one();
	consumer_lock.unlock();
	return item;
}

/**
 * Adds a new item to the back of the buffer.
 *
 * @param new_item The item to put in the buffer.
 */
void BoundedBuffer::putItem(int new_item) {
	std::unique_lock<std::mutex> producer_lock(this->shared_mutex);

	//this->space_available.wait(producer_lock);
	buffer.push(new_item);
	this->data_available.notify_one();
	producer_lock.unlock();
}



int main(int argc, char** argv) {
	/* Make sure the user called our program correctly. */
	if (argc != 3) {
		cout << "INCORRECT USAGE!\n";
		exit(1);
	}


	/* Read the ort number from the first command line argument. */
	int port = std::stoi(argv[1]);
	/* Create a socket and start listening for new connections on the
	 * specified port. */
	int server_sock = createSocketAndListen(port);

	/* Now let's start accepting connections. */
	acceptConnections(server_sock);
	close(server_sock);

	return 0;
}

/**
 * Sends message over given socket, raising an exception if there was a problem
 * sending.
 *
 * @param socket_fd The socket to send data over.
 * @param data The data to send.
 * @param data_length Number of bytes of data to send.
 */

void sendData(int socked_fd, const char *data, size_t data_length) {
	while(data_length > 0){

		int num_bytes_sent = send(socked_fd, data, data_length, 0);
		if (num_bytes_sent == -1) {
			std::error_code ec(errno, std::generic_category());
			throw std::system_error(ec, "send failed");
		}
		data_length -= num_bytes_sent;
		data += num_bytes_sent;//Calculates buffers next position
	}
}

/**
 * Receives message over given socket, raising an exception if there was an
 * error in receiving.
 *
 * @param socket_fd The socket to send data over.
 * @param dest The buffer where we will store the received data.
 * @param buff_size Number of bytes in the buffer.
 * @return The number of bytes received and written to the destination buffer.
 */
int receiveData(int socked_fd, char *dest, size_t buff_size) {

	int num_bytes_received = recv(socked_fd, dest, buff_size, 0);
	if (num_bytes_received == -1) {
		std::error_code ec(errno, std::generic_category());
		throw std::system_error(ec, "recv failed");
	}
	return num_bytes_received;
}

/**
 * Receives a request from a connected HTTP client and sends back the
 * appropriate response.
 *
 * @note After this function returns, client_sock will have been closed (i.e.
 * may not be used again).
 *
 * @param client_sock The client's socket file descriptor.
 */
void handleClient(const int client_sock) {
	// Step 1: Receive the request message from the client

	char received_data[2048];
	int bytes_received = receiveData(client_sock, received_data, 2048);
	// Turn the char array into a C++ string for easier processing.
	string request_string(received_data, bytes_received);

	//Makes variable for easier function calling later on
	std::string f= "WWW/";


	
	std::regex http_request_regex("GET[\\s]+/([a-zA-Z0-8_/\\-\\.]*) HTTP/[0-9]\\.[0-9]\r\n", std::regex_constants::ECMAScript);	
	std::smatch request_match;
	
	int location = request_string.find("/");
	std::string buffer = request_string.substr(location+1, request_string.find("HTTP")-location-2);
	f.append(buffer);
	
	//request was not formatted correctly
	if (!std::regex_search(request_string, request_match, http_request_regex)) {	
		badRequest(client_sock);
	}
	//file was not found
	else if (!fs::exists(f)) { 
		DNE(client_sock);
	}
	//directory request
	else if(f.back()=='/'){	
			sendOK(client_sock);
			directoryHelper(client_sock, f);
	}	
	//file is found
	else {
		sendOK(client_sock);
		header(f, client_sock);
		content(f, client_sock);
	}	

	// Close connection with client.
	close(client_sock);



}

/**
 * Creates a new socket and starts listening on that socket for new
 * connections.
 *
 * @param port_num The port number on which to listen for connections.
 * @returns The socket file descriptor
 */
int createSocketAndListen(const int port_num) {
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("Creating socket failed");
		exit(1);
	}

	/* 
	 * A server socket is bound to a port, which it will listen on for incoming
	 * connections.  By default, when a bound socket is closed, the OS waits a
	 * couple of minutes before allowing the port to be re-used.  This is
	 * inconvenient when you're developing an application, since it means that
	 * you have to wait a minute or two after you run to try things again, so
	 * we can disable the wait time by setting a socket option called
	 * SO_REUSEADDR, which tells the OS that we want to be able to immediately
	 * re-bind to that same port. See the socket(7) man page ("man 7 socket")
	 * and setsockopt(2) pages for more details about socket options.
	 */
	int reuse_true = 1;

	int retval; // for checking return values

	retval = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_true,
			sizeof(reuse_true));

	if (retval < 0) {
		perror("Setting socket option failed");
		exit(1);
	}

	/*
	 * Create an address structure.  This is very similar to what we saw on the
	 * client side, only this time, we're not telling the OS where to connect,
	 * we're telling it to bind to a particular address and port to receive
	 * incoming connections.  Like the client side, we must use htons() to put
	 * the port number in network byte order.  When specifying the IP address,
	 * we use a special constant, INADDR_ANY, which tells the OS to bind to all
	 * of the system's addresses.  If your machine has multiple network
	 * interfaces, and you only wanted to accept connections from one of them,
	 * you could supply the address of the interface you wanted to use here.
	 */
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port_num);
	addr.sin_addr.s_addr = INADDR_ANY;

	/* 
	 * As its name implies, this system call asks the OS to bind the socket to
	 * address and port specified above.
	 */
	retval = bind(sock, (struct sockaddr*)&addr, sizeof(addr));
	if (retval < 0) {
		perror("Error binding to port");
		exit(1);
	}

	/* 
	 * Now that we've bound to an address and port, we tell the OS that we're
	 * ready to start listening for client connections. This effectively
	 * activates the server socket. BACKLOG (a global constant defined above)
	 * tells the OS how much space to reserve for incoming connections that have
	 * not yet been accepted.
	 */
	retval = listen(sock, BUFFER_CAPACITY);
	if (retval < 0) {
		perror("Error listening for connections");
		exit(1);
	}

	return sock;
}


void chooseThread( BoundedBuffer &buffer){
	while(true){

		int client_sock = buffer.getItem();
		handleClient(client_sock);
	}
}

/**
 * Sit around forever accepting new connections from client.
 *
 * @param server_sock The socket used by the server.
 */
void acceptConnections(const int server_sock) {
	BoundedBuffer buff(BUFFER_CAPACITY);

	for (int i = 0; i < NUMTHREADS; ++i) {
		thread connection (chooseThread, std::ref(buff));


		connection.detach();
	}          

	while (true) {
		// Declare a socket for the client connection.
		int sock;

		/* 
		 * Another address structure.  This time, the system will automatically
		 * fill it in, when we accept a connection, to tell us where the
		 * connection came from.
		 */
		struct sockaddr_in remote_addr;
		unsigned int socklen = sizeof(remote_addr);

		/* 
		 * Accept the first waiting connection from the server socket and
		 * populate the address information.  The result (sock) is a socket
		 * descriptor for the conversation with the newly connected client.  If
		 * there are no pending connections in the back log, this function will
		 * block indefinitely while waiting for a client connection to be made.
		 */
		sock = accept(server_sock, (struct sockaddr*) &remote_addr, &socklen);
		if (sock < 0) {
			perror("Error accepting connection");
			exit(1);
		}

		/* 
		 * At this point, you have a connected socket (named sock) that you can
		 * use to send() and recv(). The handleClient function should handle all
		 * of the sending and receiving to/from the client.
		 *
		 * handleClient is called by a separate thread by putting sock 
		 * into a shared buffer that is synched using condition variables
		 */
		
		buff.putItem(sock);

	}
}


/**
 * sends HTTP 400 BAD REQUEST 
 *
 * @param client_sock Client's socket file descriptor
 */

void badRequest(int client_sock){
	std::string statement = "HTTP/1.0 400 BAD REQUEST\r\n\r\n";
	sendData(client_sock, statement.c_str(), statement.length());//Sends response first, header and data in separate functions
}

/**
 * Sends HTTP 404 NOT FOUND response
 *
 * @param client_sock Client's socket file descriptor
 */

void DNE(int client_sock){
	std::string statement = "HTTP/1.0 404 Not Found\nContent-Type:text/html\nContent-Length:100\n\n<html><head><title>Uh-Oh! Page not found!</title></head><body>404 Page Not Found!</body></html>";
	sendData(client_sock, statement.c_str(), statement.length());
}

/**
 * Sends relevant HTTP headers, not including file data.
 *
 * @param client_sock Client's socket file descriptor
 * @param file_name Requested file that we are sending
 */
void header(string file_name, const int client_sock ) {
	std::string file_type;
	std::string ext = file_name.substr(file_name.find_last_of(".")+1, string::npos);
	
	//if statements to figure out file type, most common are here, but more
	//can be added if needed
	if (ext == "html")
	{
		file_type = "text/html";
	}
	else if (ext == "css")
	{
		file_type = "text/css";
	}
	else if (ext == "jpg")
	{
		file_type = "image/jpeg";
	}
	else if (ext == "gif")
	{
		file_type = "image/gif";
	}
	else if (ext == "png")
	{
		file_type = "image/png";
	}
	else if (ext == "pdf") 
	{
		file_type = "application/pdf";
	}
	else {
		file_type = "text/plain"; 
	}


	std::string size = std::to_string(fs::file_size(file_name));

	std::string response = "Content-Type:" + file_type + "\nContent-Length:" + std::to_string(fs::file_size(file_name)) + "\n\n";

	sendData(client_sock, response.c_str(), response.length());
}

/**
 *Sends requested file, different from headers

	@param client_sock Client's socket file descriptor
	@param file_name Requested file to send
	*/
void content(string file_name, const int client_sock) {
	//open file, place data into buffer, and send
	std::ifstream file(file_name.c_str(), std::ios::binary);

	const unsigned int buffer_size = 4096;
	char file_data[buffer_size];
	int bytes_read;
	//keep reading until EOF
	while (!file.eof()) {
		file.read(file_data, buffer_size); //Read up to buffer_size bytes into data buffer
		bytes_read = file.gcount();
		sendData(client_sock, file_data, bytes_read);
		memset(file_data, 0, sizeof(file_data));
	}
}

/**
 * formats and displays a directory 
 *
 * @param socked_fd is the socket id for the server to send the message to
 * @param dir_name is the name of the directory to be displayed
 **/
void showDirectory(int client_socket, std::string dir_name){
	std::string msg = "";
	std::string name;
	for(auto& entry: fs::directory_iterator(dir_name.c_str())) {
		name = entry.path().filename().c_str();
		msg += "<li><a href=\"" + name;
		if(fs::is_directory(dir_name + name)){
			msg.append("/"); 
		}
		msg += "\">" + name + "</a></li>";
	}
	
	std::string header = "Content-Type:text/html\nContent-Length:" + std::to_string(msg.length()+50) + "\n\n<html><body><ul>" + msg + "</ul></body></html>";
	
	sendData(client_socket, header.c_str(), header.length());
}

/*
 * checks to see if directory has an index file
 * @param socked_fd is the socket id for the server to send the message to
 */
void directoryHelper(int client_socket, string dir_name){
	std::string index = dir_name + "index.html";

	if(fs::exists(index)){
		header(index, client_socket); 
		content(index, client_socket);
	}
	else{
		showDirectory(client_socket, dir_name); 
	}
}
//Simple integer to string converter
//@param x desired integer to convert
string int_to_str(int x) {
	std::stringstream ss;
	ss << x;

	return ss.str();
}

/**
 * Sends HTTP 200 OK response
 *
 * @param client_sock Clients socket file descriptor
 */
void sendOK(const int client_sock) {
	std::string rs = "HTTP/1.0 200 OK\n";
	sendData(client_sock, rs.c_str(), rs.length());
}
