import os
import subprocess
import time

def check_result(file_path):
    with open(file_path) as result:
        with open("result.txt") as exec_result:
            if any(map(lambda x: abs(float(x[0]) - float(x[1])) > 1, zip(exec_result.readlines(), result.readlines()))):
                raise RuntimeError("not correct")
    os.remove("result.txt")


def test_default():
    print("start test_default")
    os.symlink("tests/test_default.txt", "test.txt")
    master_proc = subprocess.Popen(["./master"])
    assert(master_proc.wait() == 0)
    check_result("tests/result.txt")
    os.remove("test.txt")


def test_with_one_alive():
    print("start test_with_one_alive")
    os.symlink("tests/test_with_one_alive.txt", "test.txt")
    master_proc = subprocess.Popen(["./master"])
    assert(master_proc.wait() == 0)
    check_result("tests/result.txt")
    os.remove("test.txt")


def test_one_dead_but_return():
    print("start test_one_dead_but_return")
    os.symlink("tests/test_one_dead_but_return.txt", "test.txt")
    master_proc = subprocess.Popen(["./master"])
    assert(master_proc.wait() == 0)
    check_result("tests/result.txt")
    os.remove("test.txt")


def main():
    if os.path.exists("test.txt"):
        os.remove("test.txt")
    test_default()
    test_with_one_alive()
    test_one_dead_but_return()


if __name__ == "__main__":
    main()
