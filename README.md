# simple-kanban
基于C语言Socket编程的简易公告发布程序

# 编译

仅适用于`unix`类系统，`windows`下编译请自行将`socket`实现替换为`winsock`

```bash
git clone https://github.com/fumiama/simple-kanban.git
cd simple-kanban
mkdir build
cd build
cmake ../
make
```

# 数据格式

1. 看板、数据的报文头部有固定的4字节，用来标识之后数据的长度，但回复串（succ/erro/null/get/set）无此头部。
2. PASSWORD、SETPASS位于`server.c`头部，可自行定义。

# 使用

0. 启动程序

```bash
./simple-kanban -d 7777 1 ./kanban.txt ./data.bin
```

1. 用`PASSWORD`连接，连接后10秒无下一步操作自动断开

- 发送：`PASSWORD`字串

2. 获得看板

- 发送：get
- 返回：get
- 发送：版本号（一串数字）
- 返回：看板（有新消息）/`null`（无新消息）

3. 获得数据

- 发送：cat
- 返回：数据

4. 设置看板

- 发送：set
- 返回：set
- 发送：ver+`SETPASS`
- 返回：data
- 发送：新的看板
- 返回：succ

5. 设置数据

- 发送：set
- 返回：set
- 发送：dat+`SETPASS`
- 返回：data
- 发送：新的数据
- 返回：succ