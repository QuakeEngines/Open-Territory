#!/bin/sh

echo "/* Generated list of glue data for XreaL glsl shaders */"
echo
echo "#ifndef XBEGIN"
echo "#define XBEGIN(x,y)"
echo "#endif"
echo "#ifndef XVARYING"
echo "#define XVARYING(a,b,c,d)"
echo "#endif"
echo "#ifndef XUNIFORM"
echo "#define XUNIFORM(a,b,c,d)"
echo "#endif"
echo "#ifndef XATTRIBUTE"
echo "#define XATTRIBUTE(a,b,c,d)"
echo "#endif"
echo "#ifndef XDEFINE"
echo "#define XDEFINE(a,b)"
echo "#endif"
echo "#ifndef XEND"
echo "#define XEND(x,y)"
echo "#endif"
echo ""

for i in `ls $1/*.glsl`
do
	name=`basename "$i"`
	echo "/* $name */"

	PP=`grep -Ev "#(extension|version)" $i | sed 's/;/\n/'`

	echo "$name\n$PP" | awk '
		/\.glsl/ { 
			if (!once) {
				once = 1
				gsub(/\.glsl.*/,""); 
				filename = $0; 
				split(filename, parts, "_"); 
				shader = substr(filename, 1, length(filename) - length(parts[length(parts)])-1);
				type = parts[length(parts)];
				printf "\nXBEGIN(%s, %s)\n", shader, type
			}
		}
		/defined[ \t\v\f]*\([ \t\v\f]*[^)]*[ \t\v\f]*\)/ {
			match($0,/defined[ \t\v\f]*\([ \t\v\f]*(USE_[^)]*)[ \t\v\f]*\)/,defines);
			count = length(defines)
			for (i = 1; i <= count; i++) {
				nogo = 0;
				for (j = 1; j <= oldcount; j++) {
					if (defines[i] == olddefines[j]) {
						nogo = 1;
						break;
					}
				}
				if (defines[i] != "" && nogo == 0) {
					printf "XDEFINE(%s, %s)\n", shader, defines[i]
				}
			}
			for (i = 1; i <= count; i++)
				olddefines[oldcount + i] = defines[i];
			oldcount = oldcount + count;
		}
		/^(uniform|attribute)/ {
			printf "X%s(%s, %s, %s, %s)\n", toupper($1), shader, type, $2, $3
		}
		END {
			printf "XEND(%s, %s)\n\n", shader, type
		}
	'
	#echo "$PP" | grep -E "^(varying|uniform|attribute)" | sed 's|varying[ \t\v\f]\+|VARYING(|' | sed 's|[ \t\v\f]*$|)|' | sed 's|[ \t\v\f]\+|, |'
done

