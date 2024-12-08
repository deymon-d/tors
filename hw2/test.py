import random
import requests
import time
import unittest

class TestMethods(unittest.TestCase):
    def get_master(self):
        server = random.sample(self.servers, 1)[0]
        result = requests.get(
            f"{server}/state", allow_redirects=True
        )
        server = f"{result.json()['master']}:5000"
        return server

    def __init__(self, *args, **kwargs):
        super(TestMethods, self).__init__(*args, **kwargs)
        self.servers = {
            "http://raft-server-1:5000",
            "http://raft-server-2:5000",
            "http://raft-server-3:5000",
            "http://raft-server-4:5000",
            "http://raft-server-5:5000"
        }

    def test_usual_case(self):
        server = random.sample(self.servers, 1)[0]
        data = {
            "key": "_my_key_123",
            "value": "valuev_123"
        }

        result = requests.post(
            f"{server}/values", json=data, allow_redirects=True
        )

        print(result.history)
        assert result.status_code == 200

        server = random.sample(self.servers, 1)[0]
        result = requests.get(
            f"{server}/values/{data['key']}",
            allow_redirects=True
        )

        print(result.history)
        assert result.status_code == 200
        assert result.json() == data

        new_data = {
            "value": "new_valuev_123"
        }
        server = random.sample(self.servers, 1)[0]
        result = requests.put(
            f"{server}/values/{data['key']}", json=new_data, allow_redirects=True
        )
        print(result.history)

        assert result.status_code == 200
        data.update(new_data)
        print(data)
        print(result.json())
        assert result.json() == data
        server = random.sample(self.servers, 1)[0]
        result = requests.delete(
            f"{server}/values/{data['key']}", json=new_data, allow_redirects=True
        )
        print(result.history)

        assert result.status_code == 200
        server = random.sample(self.servers, 1)[0]
        result = requests.get(
            f"{server}/values/{data['key']}", allow_redirects=True
        )
        print(result.history)

        assert result.status_code == 404
        result = requests.get(
            f"{server}/state", allow_redirects=True
        )   
        server = f"{result.json()['master']}:5000"
        result = requests.get(
            f"{server}/values/{data['key']}", allow_redirects=True
        )
        print(result.history)

        assert result.status_code == 404

    
    def test_one_kill(self):
        server = self.get_master()
        self.assertEqual(requests.post(f"{server}/kill").status_code, 200)
        time.sleep(20)
        self.servers.remove(server)
        self.test_usual_case()
        self.assertEqual(requests.post(f"{server}/recovery").status_code, 200)
        self.servers.add(server)

    def test_two_kill(self):
        server = self.get_master()
        self.assertEqual(requests.post(f"{server}/kill").status_code, 200)
        time.sleep(20)
        self.servers.remove(server)
        self.test_one_kill()
        self.assertEqual(requests.post(f"{server}/recovery").status_code, 200)
        self.servers.add(server)

    def test_three_kill(self):
        servers = []
        for i in range(3):
            server = self.get_master()
            self.assertEqual(requests.post(f"{server}/kill").status_code, 200)
            time.sleep(20)
            servers.append(server)
            self.servers.remove(server)
        server = random.sample(self.servers, 1)[0]
        result = requests.get(f"{server}/state")
        print(result.json())
        self.assertTrue(result.json()["master"] is None)
        for server in servers:
            self.assertEqual(requests.post(f"{server}/recovery").status_code, 200)
            self.servers.add(server)
        time.sleep(20)
        self.test_usual_case()


if __name__ == '__main__':
    time.sleep(20)
    unittest.main()
