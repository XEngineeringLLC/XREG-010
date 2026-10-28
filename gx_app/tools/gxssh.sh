#!/bin/zsh
# gxssh.sh 'command' : run a command on the bench GX as root (bench part, password 00000000, host key not pinned)
sshpass -p 00000000 ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 root@${GX:-172.20.10.2} "$@"
