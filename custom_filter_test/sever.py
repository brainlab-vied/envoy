from flask import Flask, request, jsonify
import logging

app = Flask(__name__)

@app.route('/endpoints.Greeter/SayHello', methods=['POST'])
def json_endpoint():
    print('data')
    try:
         print('data')
         print(request.data)
         print('headers')
         print(request.headers)
         #data = request.get_json()  # Get JSON data from the request
         # Process the JSON data as needed
         result = {"message": "aca"}
         print(data)
         return jsonify(result), 200
    except Exception as e:
         error_message = {"error": "Error processing JSON data", "details": str(e)}
         return jsonify(error_message), 400

if __name__ == '__main__':
    print('aaaaaaaaaaaa')
    app.run(host="0.0.0.0")
