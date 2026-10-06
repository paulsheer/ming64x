
D=../

v=`egrep '[0-9][0-9]*[.][0-9][0-9]*[.][0-9][0-9]*[.][0-9][0-9]*' $D/MICROSOFT-STORE | awk '{print $4;exit;}'`
vc=`echo $v | sed -e 's/[.]/,/g'`
d=`egrep '[0-9][0-9]*[.][0-9][0-9]*[.][0-9][0-9]*[.][0-9][0-9]*' $D/MICROSOFT-STORE | awk '{print $1" "$2" "$3;exit;}'`

grep -qs "^ -e [']s/@MICROSOFT_STORE_CURRENT_VERSION@/$v/g['] \\\\\$" $D/MICROSOFT-STORE-CURRENT-VERSION || { echo error1 ; exit 1 ; }
grep -qs "^ -e [']s/@MICROSOFT_STORE_COMMA_VERSION@/$vc/g['] \\\\\$" $D/MICROSOFT-STORE-CURRENT-VERSION || { echo error2 ; exit 1 ; }
grep -qs "^ -e [']s/@MICROSOFT_STORE_DATE_VERSION@/$d/g[']\$" $D/MICROSOFT-STORE-CURRENT-VERSION || { echo error3 ; exit 1 ; }

echo correct
exit 0


