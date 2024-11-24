#include <arpa/inet.h>
#include <assert.h>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <iostream>
#include <netdb.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#include "constants.h"

static std::atomic<size_t> GLOBAL_ID_COUNTER = 0;

class TResponse {
public:
    TResponse(double value)
        : State(EState::OK)
        , Value(value)
    {}

    TResponse(std::string error)
        : State(EState::ERROR)
        , Error(std::move(error))
    {}

    TResponse(const char* str)
            : State(EState::ERROR)
            , Error(str)
    {}

    TResponse()
        : State(EState::ERROR)
        , Error("no value in ctor")
    {}

    bool IsError() {
        return State == EState::ERROR;
    }

    bool IsSuccess() {
        return State == EState::OK;
    }

    std::string GetError() {
        assert(IsError());
        return Error;
    }

    double GetValue() {
        assert(IsSuccess());
        return Value;
    }

private:
    enum EState {
        OK,
        ERROR,
    };

    EState State;
    double Value;
    std::string Error;
};

struct TTask {
    TTask(double left, double right)
        : Left(left)
        , Right(right)
        , ID(GLOBAL_ID_COUNTER++) {}

    size_t ID;
    double Left;
    double Right;

    friend std::ostream& operator<<(std::ostream& out, const TTask& task) {
        out << "ID: " << task.ID << ", left: " << task.Left << ", right: " << task.Right << std::endl;
        return out;
    }
};


class TServerDiscoverer {
public:
    struct ServerInfo {
        ServerInfo(std::string ip, bool dead): IP(std::move(ip)), Dead(dead) {}
        std::string IP;
        bool Dead;
    };

    std::vector<ServerInfo> GetServers() {
        int broadcastSock = socket(AF_INET, SOCK_DGRAM, 0);
        if (broadcastSock < 0) {
            std::cerr << "creating broadcast socket failed" << std::endl;
            return {};
        }

        sockaddr_in broadcastAddr = {};
        broadcastAddr.sin_family = AF_INET;
        broadcastAddr.sin_port = htons(BROADCAST_PORT);
        broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;
        int broadcast = 1;
        if (setsockopt(broadcastSock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
            std::cerr << "Setting broadcast option failed" << std::endl;
            close(broadcastSock);
            return {};
        }
        std::vector<ServerInfo> servers;
        const char* message = DISCOVERY_MESSAGE.c_str();
        if (sendto(broadcastSock, message, strlen(message), 0,
                   (struct sockaddr*)&broadcastAddr, sizeof(broadcastAddr)) < 0) {
            std::cerr << "Error sending broadcast message from master" << std::endl;
        }
        ReceiveResponses(broadcastSock, servers);
        std::cout << "find servers:\n";
        for (const auto& server : servers) {
            std::cout << "IP: " << server.IP << std::endl;
        }
        close(broadcastSock);
        std::cerr << servers.size() << std::endl;
        return servers;
    }

private:
    void ReceiveResponses(int broadcastSock, std::vector<ServerInfo>& servers) {
        char buffer[256];
        sockaddr_in clientAddr;
        socklen_t clientAddrLen = sizeof(clientAddr);
        fd_set readFds;
        struct timeval timeout;
        while (true) {
            FD_ZERO(&readFds);
            FD_SET(broadcastSock, &readFds);
            timeout.tv_sec = TIMEOUT_DISCOVERY_SEC;
            timeout.tv_usec = 0;
            int ret_code = select(broadcastSock + 1, &readFds, nullptr, nullptr, &timeout);
            if (ret_code < 0) {
                std::cerr << "Select failed" << std::endl;
                break;
            }
            if (ret_code == 0) {
                std::cout << "No response received" << std::endl;
                break;
            }

            if (FD_ISSET(broadcastSock, &readFds)) {
                int bytesReceived = recvfrom(broadcastSock, buffer, sizeof(buffer), 0,
                                             (struct sockaddr*)&clientAddr, &clientAddrLen);
                if (bytesReceived < 0) {
                    std::cerr << "Error receiving broadcast response!" << std::endl;
                    continue;
                }
                buffer[bytesReceived] = '\0';
                std::string response(buffer);
                if (response == DISCOVERY_RESPONSE) {
                    std::string ipAddress = inet_ntoa(clientAddr.sin_addr);
                    servers.emplace_back(ipAddress, false);
                }
            }
        }
    }
};

class TMaster {
public:
    TMaster() {
        std::thread t([this](){
            Servers = Discoverer.GetServers();
            while (true) {
                    std::this_thread::sleep_for(LOOP_DISCOVERY_TIME);
                    std::lock_guard lock(mtx);
                    Servers = Discoverer.GetServers();
                }
            });
        t.detach();
    }

    double ExecuteTask(double left, double right) {
        double step = (right - left) / MASTER_SPLIT_COUNT;

        std::deque<TTask> tasks;
        for (size_t i = 0; i < MASTER_SPLIT_COUNT; ++i) {
            tasks.emplace_back(left + step * i, left + step * (i + 1));
        }
        double result = 0.0;
        while (!tasks.empty()) {
            std::vector<std::pair<std::future<TResponse>, TTask>> results;
            while (!tasks.empty()) {
                auto task = tasks.front();
                tasks.pop_front();
                results.emplace_back(Schedule(task), task);
            }

            for (auto& [res, task]: results) {
                auto status = res.wait_for(std::chrono::milliseconds(10));
                if (status == std::future_status::timeout) {
                    std::cerr << "errored task: " << task << "Error: timeout" << std::endl;
                    tasks.push_back(task);
                    continue;
                }
                auto response = res.get();
                if (response.IsSuccess()) {
                    result += response.GetValue();
                } else if (response.IsError()) {
                    std::cerr << "errored task: " << task << "Error: " << response.GetError() << std::endl;;
                    tasks.push_back(task);
                }
            }
        }
        return result;
    }

    void Kill(size_t count, size_t time) {
        std::string serializedKill = DIE_MESSAGE + "\n" + std::to_string(time) + "\n";
        size_t killed = 0;
        for (size_t i = 0; i < Servers.size() && killed < count; ++i) {
            if (Servers[i].Dead) {
                continue;
            }
            auto socket = ConnectTo(i);
            if (socket == -1) {
                continue;
            }
            send(socket, serializedKill.c_str(), serializedKill.length(), 0);
            killed += 1;
        }
        std::cout << "Successfully kill " << killed << " servers" << std::endl;
    }

private:
    std::future<TResponse> Schedule(const TTask& task) {
        auto taskLambda = [&, task]() -> TResponse {
            mtx.lock();
            if (Servers.empty()) {
                mtx.unlock();
                return "server pool is empty";
            }
            int server = GetRandomAliveServer();
            if (server < 0) {
                mtx.unlock();
                return "no alive servers";
            }

            mtx.unlock();

            auto response = SendTask(server, task);
            if (response.IsError()) {
                std::unique_lock<std::mutex> lock{mtx};
                Servers[server].Dead = true;
                std::cerr << "found dead server: " << server << std::endl;
                std::cerr << "task : " << task << "error: " << response.GetError() << std::endl;
            } else if (response.IsSuccess()) {
                std::cerr << "task: " << task << "result: " << response.GetValue() << std::endl;
            }
            return response;
        };

        return std::async(std::launch::async, taskLambda);
    }

    int GetRandomAliveServer() {
        std::unordered_set<std::size_t> deads;

        while (deads.size() != Servers.size()) {
            std::size_t server = random() % Servers.size();
            if (!Servers[server].Dead) {
                return server;
            }
            deads.insert(server);
        }

        return -1;
    }

    TResponse SendTask(size_t serverIdx, const TTask& task) {
        std::string serializedTask = TASK_MESSAGE + "\n" + std::to_string(task.Left) + " " + std::to_string(task.Right) + "\n";
        auto socket = ConnectTo(serverIdx);
        if (socket == -1) {
            return "Connection error";
        }
        send(socket, serializedTask.c_str(), serializedTask.length(), 0);

        char buffer[256];
        int bytes_received = recv(socket, buffer, sizeof(buffer), 0);
        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';
            return std::stod(buffer);
        } else {
            return "Connection error";
        }
    }

    int ConnectTo(size_t server) {
        std::unique_lock<std::mutex> lock{mtx};
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::cerr << "error while creating socket" << std::endl;
            return -1;
        }

        struct sockaddr_in serverAddr = {};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(PORT);
        if (inet_pton(AF_INET, Servers[server].IP.c_str(), &serverAddr.sin_addr) <= 0) {
            std::cerr << "incorrect IP: " << Servers[server].IP << std::endl;
            close(sock);
            return -1;
        }

        if (connect(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
            std::cerr << "can't connect to " << Servers[server].IP << std::endl;
            close(sock);
            return -1;
        }

        return sock;
    }

private:
    std::mutex mtx;
    std::vector<TServerDiscoverer::ServerInfo> Servers;
    TServerDiscoverer Discoverer;
};



int main() {
    std::ifstream input("test.txt");
    std::ofstream output("result.txt");
    std::string type_task;
    TMaster master;
    while (input >> type_task) {
        if (type_task == DIE_MESSAGE) {
            size_t count, time;
            input >> count >> time;
            master.Kill(count, time);
        } else {
            double left, right;
            input >> left >> right;
            auto result = master.ExecuteTask(left, right);
            std::cout << result;
            output << result << std::endl;
        }
    }
    return 0;
}