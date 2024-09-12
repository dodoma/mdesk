### [二 2024-09-10 17:37:55]


音源与手机 App 的心跳保持已经完成, 收包逻辑仅完成心跳包2字节收包，
下一步

1. 完成 contrl 类型所有收包逻辑
2. 启动各个自模块（控制、索引、文件等），并做好用户保存


libpocket 端需要对网络 clientDrop梳理，断网上报及callback，重连机制等。搞完就可以进行调试，然后业务逻辑了。

业务逻辑部分需要完善 binary 部分框架。


手机端与 WAN 下 moc server 的逻辑最后再做。


### [四 2024-09-12 10:20:57]

mdesk的收包逻辑及 bee 框架已经完成，
下一步

1. 完成 libpocket 的收包及重连逻辑
2. 对接完成 cmd_set_wifi 及 Users, Channels
3. Android 连调
