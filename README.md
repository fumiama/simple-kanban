# simple-kanban
基于C语言Socket编程的简易公告发布程序

# 编译

仅适用于类`unix`系统（包括Apple），`windows`下编译请自行将`socket`实现替换为`winsock`

```bash
git clone https://github.com/fumiama/simple-kanban.git
cd simple-kanban
mkdir build
cd build
cmake ../
make
```

# 数据格式

1. 看板、数据的报文头部有固定的4字节（小端序），用来标识之后数据的长度，但回复串（succ/erro/null/get/set）则无此头部。
2. PASSWORD、SETPASS位于`server.c`头部，可自行定义。

# 服务端使用

## 0. 启动程序

```bash
./simple-kanban [-d] 7777 1 ./kanban.txt ./data.bin
```

其中-d为可选项，如果添加，程序将以daemon状态运行。

## 1. 建立连接

连接后10秒无下一步操作自动断开。建立连接的操作一次会话只需执行一次。

- 发送：`PASSWORD`字串

## 2. 获得看板

- 发送：get
- 返回：get
- 发送：版本号（一串数字）
- 返回：头+看板（有新消息）/`null`（无新消息）

## 3. 获得数据

- 发送：cat
- 返回：头+数据

## 4. 设置看板

- 发送：set
- 返回：set
- 发送：ver+`SETPASS`
- 返回：data
- 发送：头+新的看板
- 返回：succ

## 5. 设置数据

- 发送：set
- 返回：set
- 发送：dat+`SETPASS`
- 返回：data
- 发送：头+新的数据
- 返回：succ

# 简易客户端使用

本程序自带一个简易客户端`client.c`，编译后名为`simple-kanban-client`，能够实现所有和服务端的交互功能。

## 0. 启动程序

```bash
./simple-kanban-client 127.0.0.1 7777 [-r]
```

接下来即可按照上面的交互流程开始使用。其中末尾的`-r`为可选项（程序只判断参数个数所以请务必放到末尾），表示以与本机相反的顺序发送文件长度。

## 1. 发送命令

直接输入命令，回车即可。

## 2. 发送文件

键入：file

回车，然后输入文件路径，回车即可。

## 3. 退出

键入：quit

回车即可