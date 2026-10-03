import http.client
import os
import re
import select
import subprocess
import time

def start(binary, *args, env=None):
    process = subprocess.Popen([str(binary), *map(str,args)], stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, env={**os.environ, **(env or {})})
    deadline = time.monotonic()+5
    lines=[]
    while time.monotonic()<deadline:
        if select.select([process.stdout],[],[],.05)[0]:
            line=process.stdout.readline();lines.append(line)
            match=re.search(r"listening on port (\d+)",line)
            if match:return process,int(match.group(1))
        if process.poll() is not None:break
    process.kill();process.wait()
    raise AssertionError("startup failed: "+''.join(lines))

def stop(process, timeout=5):
    if process.poll() is None:
        process.terminate()
        try: code=process.wait(timeout)
        except subprocess.TimeoutExpired:
            process.kill();process.wait();raise AssertionError("shutdown timeout")
        assert code==0,(code,process.stdout.read())

def request(port,path="/products/1",headers=None):
    connection=http.client.HTTPConnection("127.0.0.1",port,timeout=3)
    try:
        connection.request("GET",path,headers=headers or {})
        response=connection.getresponse()
        return response.status,dict(response.getheaders()),response.read().decode()
    finally: connection.close()

def eventually(predicate,timeout=10):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        if predicate():return
        time.sleep(.03)
    raise AssertionError("condition timed out")
