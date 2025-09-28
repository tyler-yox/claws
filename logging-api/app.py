from flask import Flask, request
import time
import requests


app = Flask(__name__)
device_ip: str = None
label = None

@app.route("/data", methods=['POST'])
def csi__data():
    csi = request.json
    csi['timestamp'] = time.time()
    csi['label'] = label
    print(csi)
    return {}


@app.route("/register", methods=['POST'])
def register_device_ip():
    global device_ip
    device_ip = request.json['ip']
    print(device_ip)
    return {}

@app.route("/start", methods=['POST'])
def start_counter():
    global label
    label = request.json['label']
    print(label)
    requests.get('http://' + device_ip + '/start')
    return {}


app.run(host="0.0.0.0")
