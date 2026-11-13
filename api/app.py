# ECSE3038 Test 2: the bathroom fan API
import os
import json
import secrets
from datetime import datetime, timezone
from typing import Literal

from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, Header, Query, Response
from pydantic import BaseModel, Field
from pymongo import MongoClient, ASCENDING
from pymongo.errors import DuplicateKeyError
import paho.mqtt.publish as mqtt_publish

load_dotenv()

client = MongoClient(os.getenv("MONGODB_URI"), tz_aware=True)
db = client["test2"]
fans = db["fans"]
readings = db["readings"]
settings = db["settings"]
fans.create_index([("mac", ASCENDING)], unique=True)

MQTT_NAME = os.getenv("MQTT_NAME", "")
BROKER_HOST = "135.232.203.144"
BROKER_PORT = 1883

app = FastAPI()


class FanIn(BaseModel):
    mac: str
    room: str


class ReadingIn(BaseModel):
    mac: str
    humidity: float = Field(ge=0, le=100)


class SettingsIn(BaseModel):
    mode: Literal["auto", "on", "off"]
    max_humidity: float = Field(ge=0, le=100)


# ---------- A1: fans ----------

@app.post("/fans", status_code=201)
def create_fan(fan: FanIn):
    new_fan = fan.model_dump()
    new_fan["key"] = secrets.token_urlsafe(16)
    try:
        fans.insert_one(new_fan)
    except DuplicateKeyError:
        raise HTTPException(status_code=409, detail="A fan with MAC " + fan.mac + " already exists")
    new_fan.pop("_id", None)
    return new_fan


@app.get("/fans")
def get_fans():
    return list(fans.find({}, {"_id": 0, "key": 0}))


@app.get("/fans/{mac}")
def get_fan(mac: str):
    fan = fans.find_one({"mac": mac}, {"_id": 0, "key": 0})
    if fan is None:
        raise HTTPException(status_code=404, detail="No fan with MAC " + mac)
    return fan


# ---------- A2: POST /readings ----------

@app.post("/readings", status_code=201)
def create_reading(reading: ReadingIn, x_api_key: str | None = Header(default=None)):
    if not x_api_key:
        raise HTTPException(status_code=401, detail="Missing X-API-Key")
    owner = fans.find_one({"key": x_api_key})
    if owner is None:
        raise HTTPException(status_code=401, detail="Unknown key")
    if owner["mac"] != reading.mac:
        raise HTTPException(status_code=403, detail="This key belongs to another fan")
    new_reading = reading.model_dump()
    new_reading["time"] = datetime.now(timezone.utc)
    readings.insert_one(new_reading)
    new_reading.pop("_id", None)
    return new_reading


# ---------- A3: GET /readings ----------

@app.get("/readings")
def get_readings(mac: str | None = None, limit: int = Query(default=10, ge=1, le=100)):
    query = {}
    if mac is not None:
        query["mac"] = mac
    return list(readings.find(query, {"_id": 0}).sort("time", -1).limit(limit))


# ---------- A4: PUT /fans/{mac}/settings ----------

@app.put("/fans/{mac}/settings")
def put_settings(mac: str, new_settings: SettingsIn, response: Response):
    if fans.find_one({"mac": mac}) is None:
        raise HTTPException(status_code=404, detail="No fan with MAC " + mac)

    body = new_settings.model_dump()
    result = settings.replace_one({"mac": mac}, {"mac": mac, **body}, upsert=True)
    response.status_code = 200 if result.matched_count > 0 else 201

    topic = "ecse3038/" + MQTT_NAME + "/fan/settings"
    try:
        mqtt_publish.single(topic, json.dumps(body), retain=True,
                            hostname=BROKER_HOST, port=BROKER_PORT)
    except Exception as err:
        print("MQTT publish failed:", err)

    return body


# ---------- A5: DELETE /fans/{mac} ----------

@app.delete("/fans/{mac}", status_code=204)
def delete_fan(mac: str, x_api_key: str | None = Header(default=None)):
    fan = fans.find_one({"mac": mac})
    if fan is None:
        raise HTTPException(status_code=404, detail="No fan with MAC " + mac)
    if not x_api_key or fans.find_one({"key": x_api_key}) is None:
        raise HTTPException(status_code=401, detail="Missing or unknown key")
    if fan["key"] != x_api_key:
        raise HTTPException(status_code=403, detail="This key belongs to another fan")
    fans.delete_one({"mac": mac})
    settings.delete_one({"mac": mac})
    return Response(status_code=204)
