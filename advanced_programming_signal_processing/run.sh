#!/bin/sh

# Usage: run.sh level<1-8>   (例: run.sh level1)
#   ※ 引数はテンプレートディレクトリ名を兼ねる (例: level1 という名前のディレクトリを使う)
#   level 1 : none        (正解はテンプレートの中から1つ)
#   level 2 : blur        (画像にノイズ混入)
#   level 3 : contrast    (コントラスト変化)
#   level 4 : threshold   (テンプレート背景透過: edgeモード)
#   level 5 : resize50/200(テンプレートサイズ可変)
#   level 6 : rotate      (テンプレート回転)
#   level 7 : all         (1〜6のシャッフル)
#   level 8 : contrast×resize×rotate
#
# ★バッチモード:
#   全ジョブ (画像×加工種) を1つのジョブファイルに書き出し、
#   ./matching -jobs <file> の1プロセスで処理する。
#   - 画像・テンプレートは各1回だけ読み込み
#   - (ジョブ×テンプレート) の全タスクにフラットにOpenMP並列 (全コア)
#   結果txtのセマンティクス (clear→辞書順追記) は従来と同一。

if [ -z "$1" ]; then
    echo "Usage: $0 level<1-8>  (e.g. $0 level1)" >&2
    exit 1
fi

# 引数(例: level1)をそのままテンプレートディレクトリ名として使い、
# 数字部分だけを取り出してレベル番号にする
DIR="$1"
LEVEL=$(echo "$1" | sed -e 's/^[Ll]evel//')
#level8には画像が存在しないため，テストしたかったら構造を戻して
#LEVEL="$2"

case "$LEVEL" in
    1|2|3|4|5|6|7|8) ;;
    *)
        echo "level must be level1-level8 (got: $1)" >&2
        exit 1
        ;;
esac

# -------------------------
# このレベルで各処理を実行するか
# -------------------------
do_none=0
do_blur=0
do_contrast=0
do_thresh=0
do_resize=0
do_rotate=0
do_level8=0

case "$LEVEL" in
    1) do_none=1 ;;
    2) do_blur=1 ;;
    3) do_contrast=1 ;;
    4) do_thresh=1 ;;
    5) do_resize=1 ;;
    6) do_rotate=1 ;;
    7) do_none=1; do_blur=1; do_contrast=1; do_thresh=1; do_resize=1; do_rotate=1 ;;
    8) do_level8=1 ;;
esac

echo "level -> $LEVEL"

# -------------------------
# 出力ディレクトリ
# -------------------------

mkdir -p imgproc
mkdir -p imgproc/tmp
mkdir -p result

if [ "$do_level8" = 1 ]; then
    mkdir -p result_level8
    for contrast_tag in 05 10 15 20; do
        mkdir -p "imgproc/input_contrast_${contrast_tag}"
        for size in 50 100 200; do
            for rotation in 0 90 180 270; do
                mkdir -p "result_level8/c${contrast_tag}_s${size}_r${rotation}"
            done
        done
    done
else
    [ "$do_none" = 1 ]     && mkdir -p result_none
    [ "$do_blur" = 1 ]     && mkdir -p result_blur
    [ "$do_contrast" = 1 ] && mkdir -p result_contrast
    [ "$do_thresh" = 1 ]   && mkdir -p result_edge
    [ "$do_resize" = 1 ]   && mkdir -p result_resize50 result_resize100 result_resize200
    [ "$do_rotate" = 1 ]   && mkdir -p result_rotate0 result_rotate90 result_rotate180 result_rotate270
fi

# -------------------------
# level8 テンプレート前処理（サイズ3通り×回転4通り）
# -------------------------
if [ "$do_level8" = 1 ]; then
    for size in 50 100 200; do
        for rotation in 0 90 180 270; do
            mkdir -p "imgproc/template_s${size}_r${rotation}"
        done
    done

    for template in "$DIR"/*.ppm; do
    (
        [ -e "$template" ] || exit 0
        template_name=$(basename "$template")
        for size in 50 100 200; do
            for rotation in 0 90 180 270; do
                proc_template="imgproc/template_s${size}_r${rotation}/${template_name}"
                if [ ! -e "$proc_template" ]; then
                    convert "$template" -resize "${size}%" -rotate "$rotation" "$proc_template"
                fi
            done
        done
    ) &
    done
    wait
fi


# -------------------------
# テンプレート前処理（必要なレベルのみ）
# -------------------------
if [ "$do_resize" = 1 ]; then
    mkdir -p imgproc/resized_50 imgproc/resized_200

    for template in "$DIR"/*.ppm; do
    (
        [ -e "$template" ] || exit 0
        name=$(basename "$template")
        convert -resize 50%  "$template" "imgproc/resized_50/$name"
        convert -resize 200% "$template" "imgproc/resized_200/$name"
    ) &
    done
    wait
fi

if [ "$do_rotate" = 1 ]; then
    mkdir -p imgproc/rotated_90 imgproc/rotated_180 imgproc/rotated_270
    for template in "$DIR"/*.ppm; do
        (
            [ -e "$template" ] || exit 0
            template_name=$(basename "$template")
            for rotation in 90 180 270; do
                rotated="imgproc/rotated_${rotation}/${template_name}"
                if [ ! -e "$rotated" ]; then
                    convert -rotate "$rotation" "$template" "$rotated"
                fi
            done
        ) &
    done
    wait
fi

# -------------------------
# level8 入力画像前処理（コントラスト4通り）
# -------------------------
if [ "$do_level8" = 1 ]; then
    for image in "$DIR"/final/*.ppm; do
    (
        [ -e "$image" ] || exit 0
        bname=$(basename "$image")
        for contrast_tag in 05 10 15 20; do
            case "$contrast_tag" in
                05) poly="0.5,0.25" ;;
                10) poly="1.0,0.0" ;;
                15) poly="1.5,-0.25" ;;
                20) poly="2.0,-0.5" ;;
            esac
            convert "$image" -function Polynomial "$poly" \
                "imgproc/input_contrast_${contrast_tag}/${bname}"
        done
    ) &
    done
    wait
fi

# -------------------------
# blur/contrast の入力画像変換 (全コアで前処理)
# -------------------------
if [ "$do_blur" = 1 ] || [ "$do_contrast" = 1 ]; then
    for image in "$DIR"/final/*.ppm; do
    (
        [ -e "$image" ] || exit 0
        bname=$(basename "$image")
        base=${bname%.ppm}
        [ "$do_blur" = 1 ]     && convert -median 3  "$image" "imgproc/${base}_blur.ppm"
        [ "$do_contrast" = 1 ] && convert -equalize "$image" "imgproc/${base}_contrast.ppm"
    ) &
    done
    wait
fi

# -------------------------
# ジョブファイル生成
#   1行: <image> <template_dir> <rotation> <threshold> <opt> <out_dir> <mode>
#   ※ resize100 は rotate0 と同一計算のため、level7では rotate0 のみ実行し
#     後で結果txtを複製する。
# -------------------------
JOBS="imgproc/jobs_level${LEVEL}.txt"
: > "$JOBS"
level8_threshold=${THRESHOLD:-80.0}

for image in "$DIR"/final/*.ppm; do
    [ -e "$image" ] || continue
    bname=$(basename "$image")
    base=${bname%.ppm}

    if [ "$do_level8" = 1 ]; then
        for contrast_tag in 05 10 15 20; do
            proc_image="imgproc/input_contrast_${contrast_tag}/${bname}"
            for size in 50 100 200; do
                for rotation in 0 90 180 270; do
                    template_dir="imgproc/template_s${size}_r${rotation}"
                    out_dir="result_level8/c${contrast_tag}_s${size}_r${rotation}"
                    echo "$proc_image $template_dir $rotation $level8_threshold cpg $out_dir tN" >> "$JOBS"
                done
            done
        done
    else
        [ "$do_none" = 1 ]     && echo "$image $DIR 0 0.5 cpg result_none simple" >> "$JOBS"
        [ "$do_blur" = 1 ]     && echo "imgproc/${base}_blur.ppm $DIR 0 0.80 cpg result_blur simple" >> "$JOBS"
        [ "$do_contrast" = 1 ] && echo "imgproc/${base}_contrast.ppm $DIR 0 0.80 cpg result_contrast simple" >> "$JOBS"
        [ "$do_thresh" = 1 ]   && echo "$image $DIR 0 70 cp result_edge edge" >> "$JOBS"

        if [ "$do_resize" = 1 ]; then
            echo "$image imgproc/resized_50 0 30.0 cp result_resize50 tN" >> "$JOBS"
            if [ "$do_rotate" != 1 ]; then
                echo "$image $DIR 0 30.0 cp result_resize100 tN" >> "$JOBS"
            fi
            echo "$image imgproc/resized_200 0 30.0 cp result_resize200 tN" >> "$JOBS"
        fi

        if [ "$do_rotate" = 1 ]; then
            echo "$image $DIR 0 30.0 cp result_rotate0 tN" >> "$JOBS"
            echo "$image imgproc/rotated_90 90 30.0 cp result_rotate90 tN" >> "$JOBS"
            echo "$image imgproc/rotated_180 180 30.0 cp result_rotate180 tN" >> "$JOBS"
            echo "$image imgproc/rotated_270 270 30.0 cp result_rotate270 tN" >> "$JOBS"
        fi
    fi
done

# -------------------------
# 全ジョブを1プロセスで実行 (OpenMPが全コアを使用)
# -------------------------
./matching -jobs "$JOBS"

# -------------------------
# level7: resize100 = rotate0 の結果を複製
# -------------------------
if [ "$do_resize" = 1 ] && [ "$do_rotate" = 1 ]; then
    for image in "$DIR"/final/*.ppm; do
        [ -e "$image" ] || continue
        bname=$(basename "$image")
        base=${bname%.ppm}
        cp "result_rotate0/${base}.txt" "result_resize100/${base}.txt"
    done
fi

# -------------------------
# score比較 (distance最小を採用)
# -------------------------
for image in "$DIR"/final/*.ppm; do
    [ -e "$image" ] || continue
    bname=$(basename "$image")
    base=${bname%.ppm}

    result_files=""
    if [ "$do_level8" = 1 ]; then
        for contrast_tag in 05 10 15 20; do
            for size in 50 100 200; do
                for rotation in 0 90 180 270; do
                    result_files="$result_files result_level8/c${contrast_tag}_s${size}_r${rotation}/${base}.txt"
                done
            done
        done
    else
        [ "$do_none" = 1 ]     && result_files="$result_files result_none/${base}.txt"
        [ "$do_blur" = 1 ]     && result_files="$result_files result_blur/${base}.txt"
        [ "$do_contrast" = 1 ] && result_files="$result_files result_contrast/${base}.txt"
        [ "$do_thresh" = 1 ]   && result_files="$result_files result_edge/${base}.txt"
        [ "$do_resize" = 1 ]   && result_files="$result_files result_resize50/${base}.txt result_resize100/${base}.txt result_resize200/${base}.txt"
        [ "$do_rotate" = 1 ]   && result_files="$result_files result_rotate0/${base}.txt result_rotate90/${base}.txt result_rotate180/${base}.txt result_rotate270/${base}.txt"
    fi

    existing=""
    for f in $result_files; do
        [ -e "$f" ] && existing="$existing $f"
    done

    if [ -n "$existing" ]; then
        awk '
        BEGIN { min = 999999 }
        {
            if ($7 < min) {
                min = $7
                best = $0
            }
        }
        END {
            if (best != "")
                print best
        }
        ' $existing > "result/${base}.txt"
    else
        : > "result/${base}.txt"
    fi
done

echo "All images finished."
