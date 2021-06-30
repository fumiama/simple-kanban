if [ $# -lt 6 ];
then
    echo "usage: ip port kanban.txt data.bin password setpass"
    exit
fi

kanban=`sed '/md5:/d' $3`
md5val=`md5 $4`
cat > $3 << EOF
$kanban
EOF
echo md5:${md5val##* } | tr -d '\n' >> $3
cat > cmd_seq.txt << EOF
fumiamaset$6
dat
file
$4
set$6
ver
file
$3
quit
EOF
simple-kanban-client $1 $2 < cmd_seq.txt
