#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

#include "constants.h"


class Server {
public:
    void Run() {
        std::thread server_thread([this](){this->BusinessLogic();});
        std::thread broadcast_thread([this](){this->BroadcastLogic();});
        server_thread.join();
        broadcast_thread.join();
    }

private:
    bool Dead = false;

    void BusinessLogic() {
        int server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock < 0) {
            std::cerr << "Socket creation failed" << std::endl;
            return;
        }

        sockaddr_in server_addr = {};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(PORT);
        server_addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(server_sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            std::cerr << "Bind failed" << std::endl;
            close(server_sock);
            return;
        }

        if (listen(server_sock, 5) < 0) {
            std::cerr << "Listen failed" << std::endl;
            close(server_sock);
            return;
        }
        std::cout << "Ready for calculation" << std::endl;
        while (true) {
            int client_sock = accept(server_sock, nullptr, nullptr);
            if (client_sock < 0) {
                std::cerr << "Accept failed" << std::endl;
                continue;
            }
            if (!Dead) {
                ExecuteRequest(client_sock);
            }
        }
        close(server_sock);
    }

    void ExecuteRequest(int client_sock) {
        char buffer[256];
        int bytes_received = recv(client_sock, buffer, sizeof(buffer), 0);
        if (bytes_received <= 0) {
            close(client_sock);
            return;
        }

        buffer[bytes_received] = '\0';
        std::string task(buffer);
        std::stringstream task_stream(task);
        std::string type;
        task_stream >> type;
        if (type == DIE_MESSAGE) {
            size_t time;
            task_stream >> time;
            auto sleep_time = std::chrono::seconds(time);
            close(client_sock);
            Dead = true;
            throw std::runtime_error("dead");
            Dead = false;
            return;
        }
        double left, right;
        task_stream >> left >> right;
        double result = Calculate(left, right);
        std::string result_str = std::to_string(result);
        send(client_sock, result_str.c_str(), result_str.length(), 0);
        close(client_sock);
    }

    double Calculate(double left, double right) {
        std::cerr << "Task: [" << left << ", " << right << "]" << std::endl;
        double sum = 0.0;
        double h = (right - left) / SPLIT_COUNT;

        for (int i = 0; i < SPLIT_COUNT; ++i) {
            double x = left + i * h;
            sum += x * x * x * h;
        }
        return sum;
    }

    void BroadcastLogic() {
        int broadcast_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (broadcast_sock < 0) {
            std::cerr << "Broadcast socker creation failed" << std::endl;
            return;
        }
        sockaddr_in broadcast_addr = {};
        broadcast_addr.sin_family = AF_INET;
        broadcast_addr.sin_port = htons(BROADCAST_PORT);
        broadcast_addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(broadcast_sock, (sockaddr*)&broadcast_addr, sizeof(broadcast_addr)) < 0) {
            std::cerr << "Bind failed" << std::endl;
            close(broadcast_sock);
            return;
        }
        int broadcast = 1;
        if (setsockopt(broadcast_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
            std::cerr << "Setting broadcast option failed" << std::endl;
            close(broadcast_sock);
            return;
        }

        char buffer[256];
        sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);

        std::cout << "Ready for broadcast" << std::endl;

        while (true) {
            int bytes_received = recvfrom(broadcast_sock, buffer, sizeof(buffer), 0,
                                        (sockaddr*)&client_addr, &client_addr_len);

            if (bytes_received < 0) {
                std::cerr << "Receiving failed" << std::endl;
                continue;
            }
            if (Dead) {
                continue;
            }

            buffer[bytes_received] = '\0';
            std::string message(buffer);

            if (message == DISCOVERY_MESSAGE) {
                std::string client_ip = inet_ntoa(client_addr.sin_addr);
                std::cout << "find master at " << client_ip << std::endl;

                sendto(broadcast_sock, DISCOVERY_RESPONSE.c_str(), DISCOVERY_RESPONSE.length(), 0,
                    (sockaddr*)&client_addr, client_addr_len);
            }
        }
        close(broadcast_sock);
    }
};


int main() {
    Server server;
    server.Run();
    return 0;
}
