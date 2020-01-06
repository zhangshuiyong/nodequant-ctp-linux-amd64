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

# debug nodequant in vscode launch.json

```
{
    // Use IntelliSense to learn about possible attributes.
    // Hover to view descriptions of existing attributes.
    // For more information, visit: https://go.microsoft.com/fwlink/?linkid=830387
    "version": "0.2.0",
    "configurations": [
        {
            "type": "node",
            "request": "launch",
            "name": "Launch Program",
            "skipFiles": [
                "<node_internals>/**"
            ],
            "program": "${workspaceFolder}/bin/www",
            "runtimeExecutable": "$wichnode"
        }
    ]
}
```