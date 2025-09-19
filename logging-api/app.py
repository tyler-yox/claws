from flask import Flask, request
import time


app = Flask(__name__)


@app.route("/data", methods=['POST'])
def csi__data():
    csi = request.json
    csi['timestamp'] = time.time()
    print(csi)
    return {}


app.run(host="0.0.0.0")
