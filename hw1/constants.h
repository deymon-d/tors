#pragma once

#include <string>

const auto LOOP_DISCOVERY_TIME = std::chrono::seconds(60);
const int BROADCAST_PORT = 10001;  
const int PORT = 10000;  
const int TIMEOUT_DISCOVERY_SEC = 1;
const size_t MASTER_SPLIT_COUNT = 10;
const size_t SPLIT_COUNT = 10;
const std::string DIE_MESSAGE = "DIE";
const std::string DISCOVERY_MESSAGE = "MASTER_DISCOVERY";
const std::string DISCOVERY_RESPONSE = "MASTER_DISCOVERY_RESPONSE";
const std::string TASK_MESSAGE = "TASK";
