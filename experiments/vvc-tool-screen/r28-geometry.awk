# Reduce the VTM DecoderApp-structure trace to luma leaf and partition counts.
function fail(s) { print "geometry trace: " s > "/dev/stderr"; bad=1; exit 2 }
function finish_picture() {
    if (pictures && (pixels != width*height || !nodes)) fail("incomplete picture POC " poc)
}
function role(p) {
    if (!p) return "IDR"
    if (p <= 4) return "start+" p
    if (p >= 57) return "tail+" (p-56)
    return "steady+" (p%4)
}
$1=="V" && $2=="H" {
    finish_picture()
    if (NF < 12 || $5 != 0 || $9 <= 0 || $10 <= 0) fail("invalid single-slice header")
    order=$3; poc=$4; width=$9; height=$10; pixels=0; nodes=0
    if ($7==7 || $7==8) {
        if ($6!=2) fail("IDR slice type")
        base=poc; periods++; delete seen
    } else if (!periods || $6!=0) fail("expected an IDR followed by B slices")
    relative=poc-base
    if (relative<0 || (!schedule && relative>=60) || relative in seen) fail("invalid POC in IDR period")
    seen[relative]=1; r=role(schedule ? original_rel[order] : relative); pictures++; count["pictures\t" r]++
    next
}
$1=="V" && ($2=="C" || $2=="N") {
    if (!pictures || $3!=order || $4!=0 || $5<0 || $6<0 || $7<=0 || $8<=0) fail("unmatched partition")
    shape=$7 "x" $8
    if ($2=="C") {
        if (NF!=9 || ($9!=0 && $9!=1) || $5+$7>width || $6+$8>height) fail("invalid luma leaf")
        pixels+=$7*$8
        count["cu_mode_geometry\t" r "/" ($9?"intra":"inter") "/" shape]++
    } else {
        if (NF!=11) fail("invalid split node")
        nodes++
        if ($9!=0) {
            edge=($5+$7>width || $6+$8>height)+0
            count["chosen_partition\t" r "/" shape "/split" $9 "/edge" edge "/implicit" $10 "/choices" $11]++
        }
    }
}
END {
    if (bad) exit 2
    if (!pictures) fail("no pictures")
    finish_picture()
    for (k in count) print k "\t" count[k]
    print "coverage\tpictures\t" pictures
    print "coverage\tperiods\t" periods
}
