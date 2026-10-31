"""Independent exact-arithmetic bootstrap reference; run with optional block length (default 3)."""
import sys
block = int(sys.argv[1]) if len(sys.argv) > 1 else 3
from fractions import Fraction as F
mask=(1<<64)-1
state=42
def random_start(n):
 global state
 while True:
  state=(state+0x9e3779b97f4a7c15)&mask
  z=state;z=((z^(z>>30))*0xbf58476d1ce4e5b9)&mask;z=((z^(z>>27))*0x94d049bb133111eb)&mask;x=z^(z>>31)
  if x>=((1<<64)-n)%n:return x%n
returns=[F(5,4),F(4,5),F(11,10),F(10,11)]*6
paths=[];dds=[]
for _ in range(100):
 path=[F(1)];peak=F(1);dd=F(0)
 while len(path)<25:
  start=random_start(len(returns)-block+1)
  for value in returns[start:start+block]:
   if len(path)==25:break
   path.append(path[-1]*value);peak=max(peak,path[-1]);dd=max(dd,1-path[-1]/peak)
 paths.append(path);dds.append(dd)
def q(v,p):
 a=sorted(v);pos=p*(len(a)-1);i=int(pos);return a[i]+(a[min(i+1,len(a)-1)]-a[i])*(pos-i)
print('returns',*[float(q([p[-1]-1 for p in paths],p)) for p in [F(1,20),F(1,2),F(19,20)]])
print('dd',*[float(q(dds,p)) for p in [F(1,20),F(1,2),F(19,20)]])
print('loss',sum(p[-1]<1 for p in paths)/100)
for i in [7,13,24]:print('band',i,*[float(q([p[i]*100 for p in paths],p)) for p in [F(1,20),F(1,2),F(19,20)]])
