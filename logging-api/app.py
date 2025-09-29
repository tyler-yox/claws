import threading
import time

import requests
from flask import Flask, request, render_template

app = Flask(__name__)
device_ip: str = '192.168.1.112'
current_label = {}
data_freq = {}

@app.route("/data", methods=['POST'])
def csi_data_callback():
    csi = request.json
    csi['timestamp'] = time.time()
    csi['label'] = current_label
    print(csi)
    data_freq[str(current_label)] = data_freq.get(str(current_label), 0) + 1
    return {}


@app.route("/register", methods=['POST'])
def register_device_ip():
    global device_ip
    device_ip = request.json['ip']
    print(device_ip)
    return {}


@app.route("/start", methods=['POST'])
def start_counter():
    response = {}
    global current_label
    current_label = {'room': request.json['room'], 'x': request.json['x'], 'y': request.json['y']}

    if data_freq.get(str(current_label), 0) < 10:
        try:
            threading.Thread(target=start_counter_thread).start()
        except:
            print('Failed to connect to device.')
    else:
        response['error'] = "already measured"
    return response


def start_counter_thread():
    requests.get('http://' + device_ip + '/start')


@app.route('/')
def index():
    return render_template('form.html')

app.run(host="0.0.0.0", debug=True)
