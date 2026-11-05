"""Host-test the actual bounded reader/index code without Arduino dependencies."""
from pathlib import Path
import subprocess
import tempfile
source = Path('base_station/base_station.ino').read_text()
code = source[source.index('struct HistoryPoint'):source.index('void rebuildHistoryIndex()')]
prelude = r'''
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>
#include <cassert>
#include <algorithm>
using std::isfinite;
struct File {
  std::string data; size_t pos=0, reads=0;
  uint32_t position(){return pos;}
  size_t read(uint8_t *out,size_t n){reads++;n=std::min(n,data.size()-pos);memcpy(out,data.data()+pos,n);pos+=n;return n;}
};
'''
checks = r'''
int main(){
  static_assert(sizeof(historyIndex)==1024,"Index must remain bounded");
  static_assert(sizeof(continuations)==64,"Cache must remain bounded");
  static_assert(sizeof(latestReadings)==5120,"Latest readings must remain bounded");
  for(uint32_t i=0;i<1000000;i++)indexRecord(i*3+1,i*64);
  assert(indexCount<=128);
  for(uint32_t i=0;i<1000000;i+=997){
    uint32_t id=i*3+1,offset=0;
    for(size_t j=0;j<indexCount;j++)if(historyIndex[j].id<=id)offset=historyIndex[j].offset;
    assert(offset/64<=i);assert(i-offset/64<indexStride);
  }
  File f;for(int i=0;i<10000;i++)f.data+="1,2,3,1800000000,20.00,50.00\n";
  f.data+="torn final record";
  LogReader reader(f);char line[128];int count=0;
  while(reader.line(line,sizeof(line))){
    unsigned long id,seq,ts;unsigned node;float t,h;
    assert(parseRecord(line,id,node,seq,ts,t,h));count++;
  }
  assert(count==10000);assert(f.reads<600);
  File longLine;longLine.data=std::string(1000,'x')+"\n1,2,3,4,5,6\n";
  LogReader other(longLine);assert(other.line(line,sizeof(line)));assert(line[0]==0);
  assert(other.position()==1001);assert(other.line(line,sizeof(line)));assert(std::string(line)=="1,2,3,4,5,6");
  puts("PASS: million-record sparse index, fixed RAM, buffered reads, oversized and torn lines");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp = Path(tmp) / 'test.cpp'
    exe = Path(tmp) / 'test'
    cpp.write_text(prelude + code + checks)
    subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
