import os, socket, subprocess, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import pytest
ROOT=Path(__file__).resolve().parents[1]
BINARY=os.environ.get('MINI_REDIS_BINARY',str(ROOT/'mini-redis'))
def encode(*a):
    a=[str(x).encode() if not isinstance(x,bytes) else x for x in a]
    return b'*%d\r\n'%len(a)+b''.join(b'$%d\r\n'%len(x)+x+b'\r\n' for x in a)
def read(f):
    t=f.read(1); line=f.readline().rstrip(b'\r\n')
    if t==b'+': return line
    if t==b'-': return ('error',line)
    if t==b':': return int(line)
    n=int(line)
    if t==b'$':
        if n<0:return None
        v=f.read(n); assert f.read(2)==b'\r\n'; return v
    if t==b'*':return [read(f) for _ in range(n)]
    raise AssertionError((t,line))
class Client:
    def __init__(self,p):self.s=socket.create_connection(('127.0.0.1',p));self.f=self.s.makefile('rb')
    def cmd(self,*a):self.s.sendall(encode(*a));return read(self.f)
    def close(self):self.f.close();self.s.close()
@pytest.fixture(scope='session')
def port(tmp_path_factory):
    subprocess.run(['make'],cwd=ROOT,check=True)
    s=socket.socket();s.bind(('127.0.0.1',0));p=s.getsockname()[1];s.close()
    proc=subprocess.Popen([BINARY,'--port',str(p),'--dir',str(tmp_path_factory.mktemp('rdb'))])
    for _ in range(100):
        try:c=Client(p);c.close();break
        except OSError:time.sleep(.02)
    else:raise RuntimeError('server did not start')
    yield p
    proc.terminate();assert proc.wait(timeout=5)==0
@pytest.fixture
def c(port):
    c=Client(port);c.cmd('FLUSHALL');yield c;c.close()
def test_string(c):
    assert c.cmd('GET','x') is None
    assert c.cmd('SET','x','v','EX',1)==b'OK'
    assert c.cmd('SET','x','new','NX','EX',1) is None
    assert c.cmd('GET','x')==b'v'
    assert c.cmd('TTL','x') in (0,1)
    time.sleep(1.05);assert c.cmd('GET','x') is None;assert c.cmd('TTL','x')==-2
@pytest.mark.parametrize('args',[('SET','k','v'),('SET','k','v','EX',0),('SET','k','v','EX',301),('EXPIRE','k','x'),('GET',),('LRANGE','x','a',2)])
def test_errors(c,args):assert c.cmd(*args)[0]=='error'
def test_collections(c):
    assert c.cmd('RPUSH','l','a','b','c')==3
    assert c.cmd('LRANGE','l',-2,-1)==[b'b',b'c']
    assert c.cmd('LLEN','l')==3
    assert c.cmd('GET','l')[1].startswith(b'WRONGTYPE')
    assert c.cmd('LPOP','l')==b'a'
    assert c.cmd('SADD','s','a','a','b')==2
    assert c.cmd('SISMEMBER','s','a')==1
    assert sorted(c.cmd('SMEMBERS','s'))==[b'a',b'b']
    assert c.cmd('SREM','s','a','b')==2;assert c.cmd('TTL','s')==-2
    assert c.cmd('HSET','h','f','v','g','w')==2
    assert c.cmd('HGET','h','f')==b'v'
    assert c.cmd('HGETALL','h')==[b'f',b'v',b'g',b'w']
    assert c.cmd('HDEL','h','f','g')==2;assert c.cmd('TTL','h')==-2
    assert c.cmd('ZADD','z',2,'b',1,'a',1,'c')==3
    assert c.cmd('ZRANK','z','a')==0;assert c.cmd('ZRANK','z','c')==1
    assert c.cmd('ZCARD','z')==3
    assert c.cmd('ZREM','z','a','b','c')==3;assert c.cmd('TTL','z')==-2
def test_ttl_preserved(c):
    c.cmd('RPUSH','l','x');assert c.cmd('EXPIRE','l',1)==1;c.cmd('RPUSH','l','y');time.sleep(1.05);assert c.cmd('LLEN','l')==0
    assert c.cmd('EXPIRE','no',1)==0
    c.cmd('SET','a','b','EX',2);assert c.cmd('EXPIRE','a',0)[0]=='error';assert c.cmd('GET','a')==b'b'
def test_pipeline(c):
    c.s.sendall(b''.join(encode('SET',f'k{i}','v','EX',300) for i in range(16)))
    assert [read(c.f) for _ in range(16)]==[b'OK']*16
    assert len(c.cmd('KEYS','*'))==16

def test_lua_lock(c,port):
    script="if redis.call('GET',KEYS[1]) == ARGV[1] then return redis.call('DEL',KEYS[1]) else return 0 end"
    assert c.cmd('SET','lock','owner','NX','EX',5)==b'OK'
    assert c.cmd('EVAL',script,1,'lock','other')==0
    assert c.cmd('EVAL',script,1,'lock','owner')==1
    def attempt(i):
        q=Client(port)
        try:return q.cmd('SET','lock',i,'NX','EX',5)
        finally:q.close()
    with ThreadPoolExecutor(max_workers=20) as ex:assert list(ex.map(attempt,range(20))).count(b'OK')==1

@pytest.mark.parametrize('kind,create,query,empty',[
 ('list',('RPUSH','k','a'),('LLEN','k'),0),
 ('set',('SADD','k','a'),('SMEMBERS','k'),[]),
 ('hash',('HSET','k','a','b'),('HGETALL','k'),[]),
 ('zset',('ZADD','k',1,'a'),('ZCARD','k'),0)])
def test_each_type_expiry(c,kind,create,query,empty):
    assert c.cmd(*query)==empty
    c.cmd(*create);assert c.cmd('TTL','k') in (299,300)
    assert c.cmd('EXPIRE','k',1)==1;time.sleep(1.05)
    assert c.cmd(*query)==empty

@pytest.mark.parametrize('args',[
 ('RPUSH','k'),('LPOP','k','bad'),('LLEN','k','bad'),('LRANGE','k',0),
 ('SADD','k'),('SREM','k'),('SISMEMBER','k'),('SMEMBERS','k','bad'),
 ('HSET','k','a'),('HGET','k'),('HDEL','k'),('HGETALL','k','bad'),
 ('ZADD','k','nan','a'),('ZADD','k',1,'a','bad','b'),('ZREM','k'),('ZRANK','k'),('ZCARD','k','bad'),
 ('SET','k','v','EX','9223372036854775808'),('EXPIRE','k',301),('EXPIRE','k',0),('EXPIRE','k',-1)])
def test_more_argument_boundaries(c,args):
    assert c.cmd(*args)[0]=='error';assert c.cmd('TTL','k')==-2

def test_binary_empty_and_ranges(c):
    assert c.cmd('SET',b'a\x00z',b'\x00\r\n\xff','EX',300)==b'OK'
    assert c.cmd('GET',b'a\x00z')==b'\x00\r\n\xff'
    c.cmd('RPUSH','l','','b','c')
    assert c.cmd('LRANGE','l',-999,-999)==[]
    assert c.cmd('LRANGE','l',0,9223372036854775807)==[b'',b'b',b'c']
    assert c.cmd('LRANGE','l',-9223372036854775808,-1)==[b'',b'b',b'c']
    assert c.cmd('LPOP','l')==b'';c.cmd('LPOP','l');c.cmd('LPOP','l')
    assert c.cmd('TTL','l')==-2

def test_zset_update_and_atomic_validation(c):
    c.cmd('ZADD','z',1,'b',1,'a',3,'c')
    assert c.cmd('ZRANK','z','a')==0
    assert c.cmd('ZADD','z',0,'c')==0
    assert c.cmd('ZRANK','z','c')==0
    assert c.cmd('ZADD','z',2,'new','bad','x')[0]=='error'
    assert c.cmd('ZCARD','z')==3

@pytest.mark.parametrize('frame',[b'!\r\n',b'*0\r\n',b'*1\r\n$-1\r\n',b'*1\r\n$1\r\nx!!',b'*99999999999999999999999\r\n',b'*1\n',b'*1\r\n$16777217\r\n'])
def test_malformed(port,frame):
    q=Client(port);q.s.settimeout(2);q.s.sendall(frame)
    assert read(q.f)[0]=='error';assert q.f.read(1)==b'';q.close()

def test_partial_frame(c):
    frame=encode('SET','split','value','EX',300)
    for x in frame:c.s.sendall(bytes([x]))
    assert read(c.f)==b'OK';assert c.cmd('GET','split')==b'value'

def test_lua_limits_and_capabilities(c):
    assert c.cmd('EVAL','local x=0; for i=1,10 do x=x+i end; return x',0)==55
    assert c.cmd('EVAL','local function f(x) return x+1 end; return f(2)',0)==3
    assert c.cmd('EVAL','return true',0)==1
    assert c.cmd('EVAL','return false',0) is None
    assert c.cmd('EVAL','return nil',0) is None
    for script in ["return {}", "return os.execute('id')", "return redis.call('GET','secret')", "return redis.call('KEYS','*')", 'while true do end', 'return KEYS[1][1]']:
        assert c.cmd('EVAL',script,0)[0]=='error'
    assert c.cmd('PING')==b'PONG'
    assert c.cmd('EVAL',"redis.call('SET',KEYS[1],'v','EX',300); return redis.call('LLEN',KEYS[1])",1,'changed')[0]=='error'
    assert c.cmd('GET','changed')==b'v'
    assert c.cmd('EVAL',"return redis.pcall('GET','undeclared')",0).startswith(b'script accessed')

def test_wrongtype_matrix(c):
    c.cmd('SET','k','v','EX',300)
    for args in [('RPUSH','k','a'),('LPOP','k'),('LRANGE','k',0,-1),('LLEN','k'),('SADD','k','a'),('SREM','k','a'),('SISMEMBER','k','a'),('SMEMBERS','k'),('HSET','k','f','v'),('HGET','k','f'),('HDEL','k','f'),('HGETALL','k'),('ZADD','k',1,'a'),('ZREM','k','a'),('ZCARD','k'),('ZRANK','k','a')]:
        assert c.cmd(*args)[1].startswith(b'WRONGTYPE')

def start_server(path,extra=()):
    s=socket.socket();s.bind(('127.0.0.1',0));port=s.getsockname()[1];s.close()
    proc=subprocess.Popen([BINARY,'--port',str(port),'--dir',str(path),*extra])
    for _ in range(100):
        try:return proc,Client(port)
        except OSError:
            if proc.poll() is not None:raise RuntimeError('server exited')
            time.sleep(.02)
    proc.kill();proc.wait();raise RuntimeError('startup timeout')

def test_snapshot_roundtrip_and_expiry(tmp_path):
    proc,q=start_server(tmp_path,('--snapshot-interval','1'))
    try:
        q.cmd('SET',b'bin\x00',b'val\xff','EX',300)
        q.cmd('SET','expired','gone','EX',1)
        q.cmd('RPUSH','l','a','b');q.cmd('SADD','s','x','y')
        q.cmd('HSET','h','f','v');q.cmd('ZADD','z',2,'b',1,'a')
        deadline=time.monotonic()+4
        while not (tmp_path/'dump.rdb').exists() and time.monotonic()<deadline:time.sleep(.02)
        assert (tmp_path/'dump.rdb').read_bytes().startswith(b'REDIS0009')
        time.sleep(.2);proc.kill();proc.wait();q.close()
        proc,q=start_server(tmp_path)
        assert q.cmd('GET',b'bin\x00')==b'val\xff';assert q.cmd('GET','expired') is None
        assert q.cmd('LRANGE','l',0,-1)==[b'a',b'b']
        assert sorted(q.cmd('SMEMBERS','s'))==[b'x',b'y']
        assert q.cmd('HGET','h','f')==b'v';assert q.cmd('ZRANK','z','a')==0
    finally:
        q.close()
        if proc.poll() is None:proc.terminate();assert proc.wait(timeout=5)==0

def test_memory_eviction(tmp_path):
    proc,q=start_server(tmp_path,('--maxmemory','16384'))
    try:
        for i in range(40):assert q.cmd('SET',f'k{i}',b'x'*1024,'EX',300)==b'OK'
        keys=q.cmd('KEYS','*');assert 0<len(keys)<40
        for key in keys:assert q.cmd('GET',key)==b'x'*1024
    finally:q.close();proc.terminate();assert proc.wait(timeout=5)==0

@pytest.mark.parametrize('bad',['\t1','\n1',' 1','+1','01','-0','1\x00','1 '])
def test_strict_integer(c,bad):
    assert c.cmd('SET','k','v','EX',bad)[0]=='error'
    assert c.cmd('TTL','k')==-2

def test_noncanonical_protocol_integers(port):
    for frame in [b'*\t1\r\n$4\r\nPING\r\n',b'*1\r\n$\t4\r\nPING\r\n']:
        q=Client(port);q.s.settimeout(2);q.s.sendall(frame)
        assert read(q.f)[0]=='error';assert q.f.read(1)==b'';q.close()

def test_slow_reader_pipeline_integrity(c):
    payload=b'x'*(2*1024*1024)
    assert c.cmd('SET','large',payload,'EX',300)==b'OK'
    c.s.sendall(encode('GET','large')*48)
    time.sleep(.1)
    for _ in range(48):assert read(c.f)==payload
    assert c.cmd('PING')==b'PONG'

def test_incomplete_frame_eof(port):
    q=Client(port);q.s.settimeout(2)
    q.s.sendall(encode('PING')+b'*1\r\n$5\r\nabc');q.s.shutdown(socket.SHUT_WR)
    assert read(q.f)==b'PONG';assert read(q.f)[0]=='error'
    assert q.f.read(1)==b'';q.close()

def test_eviction_hash_collision_candidates(tmp_path):
    # These names share FNV-1a bucket16815; the older entry is inside the chain.
    proc,q=start_server(tmp_path,('--maxmemory','2000'))
    try:
        q.cmd('SET','collision-575',b'a'*400,'EX',300)
        for _ in range(40):
            time.sleep(.002)
            q.cmd('SET','collision-940',b'b'*400,'EX',300)
        assert q.cmd('GET','collision-575') is None
        assert len(q.cmd('KEYS','*'))<=1
    finally:q.close();proc.terminate();assert proc.wait(timeout=5)==0

def test_reject_empty_collection_rdb(tmp_path):
    import struct
    # Redis-valid empty list encoding violates this server's no-empty-key invariant.
    data=b'REDIS0009\xfe\x00\xfc'+struct.pack('<Q',int(time.time()*1000)+300000)+b'\x01\x01k\x00\xff'+b'\0'*8
    (tmp_path/'dump.rdb').write_bytes(data)
    result=subprocess.run([BINARY,'--port','6379','--dir',str(tmp_path)],capture_output=True,timeout=3)
    assert result.returncode==1
    assert b'Invalid or unsupported RDB' in result.stderr
