import importlib.util
import threading
from pathlib import Path

spec = importlib.util.spec_from_file_location('cache_patterns', Path(__file__).resolve().parents[1]/'examples/cache_patterns.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

class Cache:
    def __init__(self): self.rows={}
    def call(self,command,key,*args):
        if command=='GET':return self.rows.get(key)
        self.rows[key]=str(args[0]).encode();return b'OK'

def test_cacheaside():
    c=Cache();db=m.MockDB({'k':{'v':1}})
    assert m.cache_aside(c,db,'k')=={'v':1}
    assert m.cache_aside(c,db,'k')=={'v':1};assert db.reads==1
    assert m.cache_aside(c,db,'missing') is None

def test_retry_success():
    db=m.MockDB(failures=2);q=m.WriteBack(Cache(),db,retry_delay=.001)
    q.put('k',{'v':1});result=q.shutdown(1)
    assert result=={'pending_failed':0,'failed_attempts':2,'exit_code':0}
    assert db.rows['k']=={'v':1}

def test_shutdown_failure_keeps_pending():
    q=m.WriteBack(Cache(),m.MockDB(always_fail=True),retry_delay=.001)
    q.put('a',1);q.put('b',2);result=q.shutdown(.03)
    assert result['pending_failed']==2;assert result['exit_code']==1
    assert len(q.pending)==2;assert result['failed_attempts']>0

def test_inflight_update_preserved():
    started=threading.Event();release=threading.Event()
    class DB(m.MockDB):
        def put(self,key,value):
            if value==1:started.set();assert release.wait(1)
            super().put(key,value)
    db=DB();q=m.WriteBack(Cache(),db);q.put('k',1);assert started.wait(1)
    q.put('k',2);release.set();assert q.shutdown(1)['exit_code']==0
    assert db.rows['k']==2
