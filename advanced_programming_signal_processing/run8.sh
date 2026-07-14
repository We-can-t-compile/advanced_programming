#!/bin/sh
# imagemagickで何か画像処理をして，/imgprocにかきこみ，テンプレートマッチング
# 最終テストは，直下のforループを次に変更 for image in $1/final/*.ppm; do

threshold=${THRESHOLD:-80.0}

for template in "$1"/*.ppm; do
    [ -e "${template}" ] || continue

    template_name=`basename "${template}"`
    (
    for size in 50 100 200; do
        for rotation in 0 90 180 270; do
            proc_dir="imgproc/template_s${size}_r${rotation}"
            proc_template="${proc_dir}/${template_name}"
            mkdir -p "${proc_dir}"

            if [ ! -e "${proc_template}" ]; then
                convert "${template}" -resize "${size}%" -rotate "${rotation}" "${proc_template}"
            fi
        done
    done
    ) &
done
wait

for image in $1/final/*.ppm; do
    [ -e "${image}" ] || continue

    (
    bname=`basename "${image}"`
    result_file="result/${bname%.ppm}.txt"
    best_distance=""
    best_line=""

    echo "$bname:"

    for contrast in 0.5 1.0 1.5 2.0; do
        case "${contrast}" in
            0.5) contrast_tag="05"; poly="0.5,0.25" ;;
            1.0) contrast_tag="10"; poly="1.0,0.0" ;;
            1.5) contrast_tag="15"; poly="1.5,-0.25" ;;
            2.0) contrast_tag="20"; poly="2.0,-0.5" ;;
        esac

        input_dir="imgproc/input_contrast_${contrast_tag}"
        mkdir -p "${input_dir}"
        name="${input_dir}/${bname}"
        convert "${image}" -function Polynomial "${poly}" "${name}"

        for template in "$1"/*.ppm; do
            [ -e "${template}" ] || continue

            template_file_name=`basename "${template}"`
            echo "${template_file_name}"
            for size in 50 100 200; do
                for rotation in 0 90 180 270; do
                    proc_template="imgproc/template_s${size}_r${rotation}/${template_file_name}"
                    output=`./matching "$name" "${proc_template}" "$rotation" 0 pg | tail -n 1`
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
