
D=../

test -f $D/MICROSOFT-STORE || exit 1

echo 'static const char szHistoryText[] ='
cat $D/MICROSOFT-STORE | \
    sed -e 's/\\/\\\\/g' -e 's/["]/\\"/g' | \
    sed -e 's/^\(.*\)$/"\1\\r\\n"/'


echo ';'

exit 0

