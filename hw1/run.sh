g++ node.cpp -pthread -o node -std=c++20
g++ master.cpp -pthread -o master -std=c++20
sudo docker-compose build
sudo docker-compose up
