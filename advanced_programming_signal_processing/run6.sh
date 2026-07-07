#!/bin/sh
# imagemagickで何か画像処理をして，/imgprocにかきこみ，テンプレートマッチング
# 最終テストは，直下のforループを次に変更 for image in $1/final/*.ppm; do

for template in "$1"/*.ppm; do
    [ -e "${template}" ] || continue

    template_name=`basename "${template}"`
    for rotation in 0 90 180 270; do
        rotated_dir="imgproc/rotated_${rotation}"
        mkdir -p "${rotated_dir}"
        rotated="${rotated_dir}/${template_name}"
        if [ ! -e "${rotated}" ]; then
            convert -rotate "${rotation}" "${template}" "${rotated}"
        fi
    done
done

for image in "$1"/test/*.ppm; do
    (
    bname=`basename "${image}"`
    name="imgproc/"$bname
    x=0    	#
    echo $name
    convert "${image}" "${name}"  # 何もしない画像処理
#   convert -blur 2x6 "${image}" "${name}"
#   convert -median 3 "${image}" "${name}"
#   convert -auto-level "${image}" "${name}"
#   convert -equalize "${image}" "${name}"

    rotation=0
    echo $bname:
    for template in "$1"/*.ppm; do
    
		template_name=`basename "${template}"`
		echo "${template_name}"
        for rotation in 0 90 180 270;do
            rotated="imgproc/rotated_${rotation}/${template_name}"
            if [ $x = 0 ]
            then
                ./matching "$name" "${rotated}" "$rotation" 30.0 cp
                x=1
            else
                ./matching "$name" "${rotated}" "$rotation" 30.0 p
            fi
        done
    done
    echo ""
    ) &
done
wait
