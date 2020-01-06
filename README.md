# nodequant-ctp-linux

# build
```
$ cd nodequant-ctp-linux-amd64
$ cp thosttraderapi_se.so /usr/local/lib/
$ cp thostmduserapi_se.so /usr/local/lib/
$ npm install node-gyp -g
$ node-gyp configure
$ node-gyp build
```
编译成功后，在当前目录会有一个build文件夹，其中Release文件夹的NodeQuant_Linux64.node就是nodequant的插件