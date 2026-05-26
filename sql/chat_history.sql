use chat;

create table if not exists chatmessage (
    id bigint primary key auto_increment,
    fromid int not null,
    toid int default null,
    groupid int default null,
    msg varchar(1000) not null,
    send_time datetime not null default current_timestamp,
    status tinyint not null default 0 comment '0=delivered, 2=offline pending',

    index idx_peer_history (fromid, toid, id),
    index idx_group_history (groupid, id)
);
