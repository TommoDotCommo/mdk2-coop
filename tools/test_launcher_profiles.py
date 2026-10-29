#!/usr/bin/env python3
"""Portable profile/store/probe tests and source mutants; no Windows process."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT/'bridge/launcher_profiles.h').read_text()
MUTANTS = {
    'nul-store': (' || memchr(data, 0, size)', ''),
    'schema': ('else return 0;\n    line', 'else version = 1;\n    line'),
    'number-overflow': ('n > (UINT64_MAX - d) / 10', '0'),
    'zero-port': ('p->port < 1 || ', ''),
    'hostname': ("if (!isalnum((unsigned char)p->host[i]) && p->host[i] != '.' && p->host[i] != '-')", 'if (0)'),
    'control-text': ('(unsigned char)s[i] < 32 || ', ''),
    'empty-name': ('(!empty && !n)', '0'),
    'store-duplicates': ('if (!strcmp(temp.p[j].name, entry->name)) return 0;', 'if (0) return 0;'),
    'encode-duplicates': ('if (!strcmp(s->p[j].name, s->p[i].name)) return 0;', 'if (0) return 0;'),
    'poll-expiry': ('>= 1200) {', '>= 12000) {'),
    'poll-rate': ('now + 15000', 'now + 1500'),
    'poll-wrap': ('(int32_t)(now - p->next) >= 0', 'now >= p->next'),
    'endpoint': ('!endpoint || ', ''),
    'pending': ('!p->pending || ', ''),
    'nonce': ('nonce != p->nonce || ', ''),
    'stale-reply': ('|| (uint32_t)(now - p->sent) >= 1200) return 0;', ') return 0;'),
    'wire-magic': ('memcmp(b, "CPS1", 4)', '0'),
    'wire-size': ('n != 12u + b[11]', '0'),
    'count': ('b[8] > b[9] || ', ''),
    'in-game': ('b[10] > 2 || ', ''),
    'version-text': ('if (b[i] < 33 || b[i] > 126)', 'if (0)'),
    'version-match': ('strcmp(p->version, version)', '0'),
    'full': ('b[8] == b[9]', '0'),
    'reachable': ('b[10] == 2 ? LP_REACHABLE : ', ''),
    'query-size': ('size != 8 || ', ''),
    'query-magic': ('memcmp(query, "CPQ1", 4)', '0'),
}
with tempfile.TemporaryDirectory() as temp:
    directory=Path(temp)
    def run(source):
        (directory/'launcher_profiles.h').write_text(source)
        subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-but-set-variable','-I',str(directory),str(ROOT/'bridge/tests/launcher_profiles_test.c'),'-o',str(directory/'test')],check=True,capture_output=True)
        return subprocess.run([str(directory/'test')],capture_output=True)
    result=run(SOURCE);assert result.returncode==0,result.stderr.decode();print(result.stdout.decode().strip())
    for name,(before,after) in MUTANTS.items():
        assert SOURCE.count(before)==1,(name,SOURCE.count(before))
        result=run(SOURCE.replace(before,after));assert result.returncode!=0,'SURVIVED: '+name
        print('KILLED:',name)

# A real loopback fake server exercises response, timeout and rogue-source paths.
import socket
import threading
with tempfile.TemporaryDirectory() as temp:
    binary=Path(temp)/'probe'
    subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-I',str(ROOT/'bridge'),str(ROOT/'bridge/tests/launcher_probe_client.c'),'-o',str(binary)],check=True)
    for name,count,game,version,expected in [('reachable',1,2,b'v1',2),('lobby',3,0,b'v1',3),('game',3,1,b'v1',4),('mismatch',3,0,b'v2',5),('full',16,0,b'v1',6),('offline',0,0,b'',1),('bad-nonce',3,0,b'v1',1),('rogue-source',3,0,b'v1',1),('truncated',3,0,b'v1',1)]:
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as server:
            server.bind(('127.0.0.1',0));server.settimeout(2)
            errors=[]
            def serve():
                try:
                    query,peer=server.recvfrom(128)
                    assert len(query)==8 and query[:4]==b'CPQ1'
                    if name=='offline':return
                    nonce=query[4:8] if name!='bad-nonce' else b'xxxx'
                    reply=b'CPS1'+nonce+bytes([count,16,game,len(version)])+version
                    if name=='truncated':reply=reply[:-1]
                    if name=='rogue-source':
                        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as rogue:rogue.sendto(reply,peer)
                    else: server.sendto(reply,peer)
                except Exception as error:errors.append(error)
            thread=threading.Thread(target=serve);thread.start()
            result=subprocess.run([str(binary),str(server.getsockname()[1]),str(expected)],capture_output=True,timeout=3)
            thread.join(3)
            assert not errors,errors
            assert not thread.is_alive()
            assert result.returncode==0,(name,result.stderr.decode())
            print('PASS: local fake server',name)

subprocess.run(["python3", str(ROOT / "tools/test_launcher_saved_join.py")], check=True)
