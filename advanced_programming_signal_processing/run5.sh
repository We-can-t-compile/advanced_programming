#!/bin/sh
# imagemagickで何か画像処理をして，/imgprocにかきこみ，テンプレートマッチング
# 最終テストは，直下のforループを次に変更 for image in $1/final/*.ppm; do

for template in "$1"/*.ppm; do
    [ -e "${template}" ] || continue

    template_name=`basename "${template}"`
    for size in 50 100 200; do
        resized_dir="imgproc/resized_${size}"
        resized="${resized_dir}/${template_name}"
        mkdir -p "${resized_dir}"
        if [ ! -e "${resized}" ]; then
            convert "${template}" -resize "${size}%" "${resized}"
        fi
    done
done

threshold=${THRESHOLD:-30.0}

for image in "$1"/test/*.ppm; do
    (
    bname=`basename "${image}"`
    name="imgproc/"$bname
    result_file="result/${bname%.ppm}.txt"
    x=0    	#

    best_distance=""
    best_line=""

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
        for size in 50 100 200;do
            resized="imgproc/resized_${size}/${template_name}"
            output=`./matching "$name" "${resized}" "$rotation" 0 p | tail -n 1`
            result_template_name=`echo "$output" | awk '{print $3}'`
            x=`echo "$output" | awk '{print $4}'`
            y=`echo "$output" | awk '{print $5}'`
            width=`echo "$output" | awk '{print $6}'`
            height=`echo "$output" | awk '{print $7}'`
            rot=`echo "$output" | awk '{print $8}'`
            distance=`echo "$output" | awk '{print $9}'`

            [ -n "$distance" ] || continue

            if [ -z "$best_distance" ] || awk "BEGIN { exit !($distance < $best_distance) }"; then
                best_distance="$distance"
                best_line="$result_template_name $x $y $width $height $rot $distance"
            fi
        done
    done

    : > "$result_file"
    if [ -n "$best_distance" ] && awk "BEGIN { exit !($best_distance < $threshold) }"; then
        echo "$best_line" > "$result_file"
        echo "[Best] $best_line"
    else
        echo "[Not found] best_distance=$best_distance"
    fi
    echo ""
    ) &
done
wait
