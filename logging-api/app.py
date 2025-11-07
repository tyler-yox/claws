import threading
import time

import requests
from flask import Flask, request, render_template

app = Flask(__name__)
device_ip: str = '192.168.1.112'
current_label = {}
data_freq = {}
error = ''


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
    global error
    current_label = {'room': request.json['room'], 'x': request.json['x'], 'y': request.json['y']}
    st = threading.Thread(target=start_counter_thread)
    started = False
    error = ''
    if data_freq.get(str(current_label), 0) < 10:
        try:
            st.start()
            started = True
        except:
            print('Failed to connect to device.')
    else:
        error = 'already measured'
    if started:
        st.join()
    if error:
        response['error'] = error
        return response, 400
    else:
        return response, 200


def start_counter_thread():
    try:
        resp = requests.get('http://' + device_ip + '/start', timeout=2)
        print(resp.text)
    except Exception:
        global error
        error = 'Failed to connect to device.'


@app.route('/')
def index():
    return render_template('form.html')

app.run(host="0.0.0.0", debug=True)
