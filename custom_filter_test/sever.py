from flask import Flask, request, jsonify
import logging

app = Flask(__name__)

@app.route('/endpoints.Greeter/SayHello', methods=['POST'])
def json_endpoint():
    try:
         print('data:')
         print(request.data)
         print('headers:')
         print(request.headers)

         result = {'message': 'hello Aca'}
         return jsonify(result), 200

    except Exception as e:
         error_message = {"error": "Error processing JSON data", "details": str(e)}
         return jsonify(error_message), 400

if __name__ == '__main__':
    app.run(host="0.0.0.0")
