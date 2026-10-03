"""Real ZooKeeper registration, replica membership and registry loss/recovery."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from rpc_process import start,stop,request,eventually

catalog_binary,gateway_binary=sys.argv[1:3]
children=[]
with tempfile.TemporaryDirectory(prefix="sylar-rpc-discovery-") as directory:
    root=Path(directory);data=root/"data";data.mkdir()
    with socket.socket() as sock:
        sock.bind(("127.0.0.1",0));zk_port=sock.getsockname()[1]
    config=root/"zoo.cfg"
    config.write_text(f"tickTime=500\ndataDir={data}\nclientPort={zk_port}\nmaxClientCnxns=30\nadmin.enableServer=false\n")
    logs=[]
    def launch_zk():
        log=(root/f"zk-{len(logs)}.log").open("w")
        logs.append(log)
        process=subprocess.Popen(["/usr/share/zookeeper/bin/zkServer.sh","start-foreground",str(config)],
            stdout=log,stderr=subprocess.STDOUT,env={**os.environ,"JMXDISABLE":"true",
            "JAVA_TOOL_OPTIONS":"-XX:-UseContainerSupport -Dzookeeper.nio.numSelectorThreads=1 -Dzookeeper.nio.numWorkerThreads=4"})
        children.append(process)
        def listening():
            assert process.poll() is None,"ZooKeeper exited"
            try:
                with socket.create_connection(("127.0.0.1",zk_port),timeout=.1):return True
            except OSError:return False
        eventually(listening)
        return process
    try:
        zk=launch_zk()
        registry={"SYLAR_ZK_HOSTS":f"127.0.0.1:{zk_port}","SYLAR_ZK_PATH":"/sylar/test/catalog"}
        a,ap=start(catalog_binary,0,"discovery-a",0,env=registry);children.append(a)
        b,bp=start(catalog_binary,0,"discovery-b",0,env=registry);children.append(b)
        gateway,port=start(gateway_binary,0,env=registry);children.append(gateway)
        view=lambda:json.loads(request(port,"/endpoints")[2])["endpoints"]
        eventually(lambda:len(view())==2)
        instances=set()
        for _ in range(8):
            code,_,body=request(port);assert code==200,body
            instances.add(json.loads(body)["instance_id"])
        assert instances=={"discovery-a","discovery-b"}
        stop(a)
        eventually(lambda:len(view())==1)
        assert json.loads(request(port)[2])["instance_id"]=="discovery-b"
        a,ap=start(catalog_binary,0,"discovery-a",0,env=registry);children.append(a)
        eventually(lambda:len(view())==2)
        a.kill();a.wait()
        eventually(lambda:len(view())==1,timeout=8)
        # While refresh blocks/reconnects, a separate expiry thread empties stale routing.
        zk.kill();zk.wait()
        eventually(lambda:not view(),timeout=3)
        assert request(port)[0]==503
        assert request(port,"/ready")[0]==503
        assert request(port,"/health")[0]==200
        time.sleep(2)
        zk=launch_zk()
        eventually(lambda:len(view())==1,timeout=20)
        eventually(lambda:request(port)[0]==200,timeout=10)
        # Server-side tracing produces a span file on graceful shutdown.
        trace_file=root/"backend-spans.json"
        c,cp=start(catalog_binary,0,"traced",0,env={**registry,"SYLAR_TRACE_FILE":str(trace_file)})
        children.append(c)
        eventually(lambda:len(view())==2)
        incoming="00-0123456789abcdef0123456789abcdef-0123456789abcdef-01"
        seen=[]
        for _ in range(8):
            code,_,body=request(port,headers={"traceparent":incoming});assert code==200
            reply=json.loads(body)
            if reply["instance_id"]=="traced":seen.append(reply["trace_parent"].split("-")[2])
        assert seen
        stop(c)
        spans=json.loads(trace_file.read_text())["spans"]
        gateway_spans=json.loads(request(port,"/traces")[2])["spans"]
        ids={span["span_id"] for span in gateway_spans}
        assert any(span["span_id"] in seen and span["parent_span_id"] in ids for span in spans)
        stop(gateway);stop(b)
        print("PASS registration, membership, ephemeral expiry, stale views, session recovery and trace tree")
    finally:
        for process in reversed(children):
            if process.poll() is None:process.terminate()
            try:process.wait(timeout=5)
            except subprocess.TimeoutExpired:process.kill();process.wait()
        for log in logs:log.close()
