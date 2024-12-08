import logging
import os
import random
import requests
import threading
import time
import uvicorn

from dataclasses import dataclass
from dataclasses_json import dataclass_json
from enum import IntEnum
from fastapi import Body, FastAPI
from fastapi.exceptions import HTTPException
from fastapi.responses import RedirectResponse
from typing import Annotated, Any, Optional

class EventType(IntEnum):
    CREATE = 0
    UPDATE = 1
    DELETE = 2

@dataclass_json
@dataclass
class Event:
    number: int
    type: EventType
    key: str
    value: Optional[Any]


class ServerStatus(IntEnum):
    SLAVE = 0
    MASTER = 1


class HealthStatus(IntEnum):
    ALIVE = 0
    DEAD = 1


NETWORK_PREFIX = "http://raft-server-"
HEARTBEAT_INTERVAL = 1
MIN_VOTE_COUNT = 3
SERVER_PORT = int(os.getenv("SERVER_PORT"))


master_address = None
server_id = int(os.getenv("SERVER_ID"))
alive_servers: set[int] = {1, 2, 3, 4, 5}
alive_servers.remove(server_id)
global_counter: int = 0
app: FastAPI = FastAPI()
storage: dict[str, Any] = dict()
events: list[Event] = list()
server_status: ServerStatus = ServerStatus.SLAVE
health_status: HealthStatus = HealthStatus.ALIVE
step: int = 0
last_heartbeat = time.time()
logger = logging.getLogger()
my_votes: dict[int, int] = dict()
servers_for_redirect: set[int] = {1, 2, 3, 4, 5}
servers_for_redirect.remove(server_id)

def check_health():
    global health_status
    if health_status != HealthStatus.ALIVE:
        raise HTTPException(503)


@app.get("/values/{key}")
def get_key_in_storage(key: str):
    check_health()
    if server_status == ServerStatus.MASTER:
        return RedirectResponse(f"{NETWORK_PREFIX}{random.sample(servers_for_redirect, 1)[0]}:5000/values/{key}")
    if key not in storage:
        raise HTTPException(404, "key: {key} not in storage")
    return {
        "key": key,
        "value": storage[key]
    }

@app.post("/values")
def create_new_kv_pair(key: Annotated[str, Body(emdeb=True)], value: Annotated[Any, Body(emdeb=True)]):
    check_health()
    if server_status != ServerStatus.MASTER:
        if master_address is None:
            raise HTTPException(423, "can't find master")
        return RedirectResponse(f"{master_address}:5000/values")
    if key in storage:
        raise HTTPException(400, "key is already in storage")
    global global_counter
    global_counter += 1
    event = Event(global_counter, EventType.CREATE, key, value)
    events.append(event)
    for slave in alive_servers:
        requests.post(
            f"{NETWORK_PREFIX}{slave}:{SERVER_PORT}/internal/values",
            json=event.__dict__
        )
    storage[key] = value
    return {
        "key": key,
        "value": storage[key]
    }

@app.post("/internal/values")
def create_new_kv_pair_from_master(event: Annotated[Event, Body(emdeb=True)]):
    check_health()
    events.append(event)
    global global_counter
    global_counter = max(global_counter, event.number)
    storage[event.key] = event.value
    return "added"

@app.put("/values/{key}")
def update_kv_pair(key: str, value: Annotated[Any, Body(embed=True)]):
    check_health()
    if server_status != ServerStatus.MASTER:
        if master_address is None:
            raise HTTPException(423, "can't find master")
        return RedirectResponse(f"{master_address}:5000/values/{key}")
    if key not in storage:
        raise HTTPException(404, "can't find key {key}")
    global global_counter
    global_counter += 1
    event = Event(global_counter, EventType.UPDATE, key, value)
    events.append(event)
    for slave in alive_servers:
        requests.put(
            f"{NETWORK_PREFIX}{slave}:{SERVER_PORT}/internal/values",
            json=event.__dict__
        )
    storage[key] = value
    return {
        "key": key,
        "value": storage[key]
    }

@app.put("/internal/values")
def create_new_kv_pair_from_master(event: Annotated[Event, Body(emdeb=True)]):
    check_health()
    events.append(event)
    global global_counter
    global_counter = max(global_counter, event.number)
    storage[event.key] = event.value
    return "updated"

@app.delete("/values/{key}")
def delete_kv_pair(key: str):
    check_health()
    if server_status != ServerStatus.MASTER:
        if master_address is None:
            raise HTTPException(423, "can't find master")
        return RedirectResponse(f"{master_address}:5000/values/{key}")
    if key not in storage:
        raise HTTPException(404, "can't find key {key}")
    global global_counter
    global_counter += 1
    event = Event(global_counter, EventType.DELETE, key, None)
    events.append(event)
    for slave in alive_servers:
        requests.delete(
            f"{NETWORK_PREFIX}{slave}:{SERVER_PORT}/internal/values",
            json=event.__dict__
        )
    del storage[key]
    return {
        "result": "ok",
    }

@app.delete("/internal/values")
def delete_kv_pair_from_master(event: Annotated[Event, Body(emdeb=True)]):
    check_health()
    events.append(event)
    global global_counter
    global_counter = max(global_counter, event.number)
    del storage[event.key]
    return "deleted"

@app.post("/kill")
def kill_server():
    global health_status
    health_status = HealthStatus.DEAD
    return {
        "result": "ok"
    }

@app.post("/recovery")
def recovery():
    global health_status
    global alive_servers
    global server_id
    global global_counter
    global server_status
    not_started = True
    while not_started:
        for server in alive_servers:
            result = requests.get(
                f"{NETWORK_PREFIX}{server}:{SERVER_PORT}/internal/recovery/{global_counter + 1}",
            )
            if result.status_code == 200:
                events_now = list(map(lambda x: Event(**x), result.json()["events"]))
                for event in events_now:
                    if event.type == EventType.CREATE or event.type == EventType.UPDATE:
                        storage[event.key] = event.value
                    elif event.type == EventType.DELETE:
                        del storage[event.key]
                    global_counter = max(global_counter, event.number)
                    events.append(event)
                global master_address
                global last_heartbeat
                last_heartbeat = time.time()
                master_address = result.json()["master"]
                health_status = HealthStatus.ALIVE
                server_status = ServerStatus.SLAVE
                not_started = False
                break
        if not_started:
            time.sleep(1)
    return {
        "result": "ok"
    }


@app.get("/internal/recovery/{counter}")
def internal_recovery(counter: int):
    check_health()
    global master_address
    if server_status != ServerStatus.MASTER and master_address is not None:
        raise HTTPException(403)
    return {
        "events": list(map(lambda x: x.__dict__, events[counter:])),
        "master": master_address
    }


@app.get("/state")
def get_state():
    return {
        "master": master_address,
        "events": list(map(lambda x: x.__dict__, events)),
        "health": health_status.name,
        "status": server_status.name,
        "storage": storage,
        "server_id": server_id,
        "step": step
    }

@app.post("/internal/vote")
def vote_for_new_master(new_step: Annotated[int, Body(emdeb=True)], new_master: Annotated[int, Body(emdeb=True)]):
    check_health()
    if new_step in my_votes:
        return {
            "result": "disapprove"
        }
    logger.info(f"server {server_id} vote for {new_master}")
    my_votes[new_step] = new_master
    global master_address
    master_address = None
    global last_heartbeat
    last_heartbeat = time.time()
    return {
        "result": "approve"
    }

@app.post("/internal/heartbeat")
def get_heartbeat(master: Annotated[int, Body(emdeb=True)], step_master: Annotated[int, Body(emdeb=True)]):
    check_health()
    global step
    if step > step_master:
        raise HTTPException(403)
    global server_status
    server_status = ServerStatus.SLAVE
    step = step_master
    global last_heartbeat
    last_heartbeat = time.time()
    global master_address
    master_address = f"{NETWORK_PREFIX}{master}"
    return {
        "result": "ok"
    }

def send_heartbeat():
    global health_status
    global server_status
    global servers_for_redirect
    while True:
        if health_status == HealthStatus.ALIVE and server_status == ServerStatus.MASTER:
            servers_for_redirect = {server for server in alive_servers}
            for slave in alive_servers:
                if requests.post(
                    f"{NETWORK_PREFIX}{slave}:{SERVER_PORT}/internal/heartbeat",
                    json={
                        "master": server_id,
                        "step_master": step
                    }
                ).status_code != 200:
                    servers_for_redirect.remove(slave)
            global last_heartbeat
            last_heartbeat = time.time()
        time.sleep(HEARTBEAT_INTERVAL)

def election_check():
    global master_address
    global health_status
    global server_status
    global last_heartbeat
    global server_id
    def election():
        global step
        global master_address
        master_address = None
        my_votes[step] = server_id
        vote_count = 1
        for server in alive_servers:
            result = requests.post(
                f"{NETWORK_PREFIX}{server}:{SERVER_PORT}/internal/vote",
                json={
                    "new_step": step,
                    "new_master": server_id
                }
            )
            if result.status_code == 200 and result.json()["result"] == "approve":
                vote_count += 1
        if vote_count >= MIN_VOTE_COUNT:
            global server_status
            server_status = ServerStatus.MASTER
            master_address = f"{NETWORK_PREFIX}{server_id}"
            logger.info("server {server_id} is elected")
    
    while True:
        if health_status == HealthStatus.ALIVE and server_status == ServerStatus.SLAVE and \
                time.time() - last_heartbeat > 3 * HEARTBEAT_INTERVAL + server_id:
            logger.info(f"server {server_id} starts election")
            election()
            global step
            step += 1
        time.sleep(HEARTBEAT_INTERVAL)


if __name__ == "__main__":
    threading.Thread(
        target=send_heartbeat, 
        daemon=True
    ).start()
    threading.Thread(
        target=election_check, 
        daemon=True
    ).start()
    uvicorn.run(app, host="0.0.0.0", port=SERVER_PORT)
