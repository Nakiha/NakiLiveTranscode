# The candidate decoder interleaves V (structure) and U (actual use) traces.
# The original instrumented decoders recorded these separately, in decode order.
function tool_fail(message) { print "R28 tool trace: " message > "/dev/stderr"; broken=1; exit 2 }
function tool_role(p) {
    if (p == 0) return "IDR"
    if (p <= 4) return "start+" p
    if (p >= 57) return "tail+" (p-56)
    return "steady+" (p%4)
}
function tool_finish() {
    if (!num) return
    if (stage != "use" && (area[0] != coded || area[1]*4 != coded || area[2]*4 != coded))
        tool_fail("incomplete transform plane at POC " tpoc)
    if (stage != "structure" && (!use_header || !uses || !used_area || amvr != inter || sbts != inter))
        tool_fail("missing CU or inter-use trace at POC " tpoc)
    if (stage == "combined" && (uses != leaves || used_area != coded))
        tool_fail("unmatched structure/use coverage at POC " tpoc)
}
function tool_header(order, p, intra) {
    tool_finish()
    if (order != num || (!num && !intra)) tool_fail("missing or unordered IDR start")
    if (intra) {
        if (stage=="combined" && num && in_period!=60)
            tool_fail("early IDR after " in_period " pictures")
        origin=p; in_period=0; delete tool_seen
    }
    rel=p-origin
    if (rel<0 || rel>=60 || rel in tool_seen) tool_fail("invalid relative POC " rel)
    tool_seen[rel]=1; tr=tool_role(rel); tpoc=p; torder=order; num++
    delete area; used_area=0; uses=0; leaves=0; inter=0; amvr=0; sbts=0; use_header=0
    print "decode_order\t" order "/" tr "\t" p
}
$1=="V" && $2=="H" {
    if (NF<12 || stage=="use") tool_fail("invalid structure header")
    tool_header($3,$4,$6==2 && ($7==7 || $7==8))
    if (NF!=12+$11+$12 || $11!~/^[0-9]+$/ || $12!~/^[0-9]+$/) tool_fail("incomplete active RPL")
    if ($6==2 ? ($11!=0 || $12!=0) : ($11==0 || $12==0)) tool_fail("invalid active reference count")
    tool_count["decode_position\t" in_period++ "/" rel]++
    delete active
    cursor=13; rpl=tr
    for (list=0;list<=1;list++) {
        refs=$(11+list); offsets=""
        for (index_in_list=0;index_in_list<refs;index_in_list++) {
            actual=$(cursor++)
            active[list,index_in_list]=actual
            offsets=offsets (index_in_list ? "," : "") (actual-tpoc)
        }
        rpl=rpl "/" (refs ? offsets : "-")
    }
    tool_count["ordered_rpl\t" rpl]++
    coded=$9*$10
}
$1=="H" {
    if (NF!=11 || stage=="structure") tool_fail("invalid use header")
    if (stage=="use") tool_header($2,$3,$4==2)
    if (!num || $2!=torder || $3!=tpoc || use_header) tool_fail("unmatched use header")
    use_header=1
}
$1=="V" && $2=="C" { leaves++ }
$1=="V" && $2=="T" {
    if (!num || $3!=torder || NF!=9 || $4!=0 || $5!~/^[012]$/ || $6<=0 || $7<=0)
        tool_fail("invalid transform block")
    tool_count["tu_geometry\t" tr "/p" $5 "/" $6 "x" $7]++
    area[$5]+=$6*$7
}
$1=="V" && $2=="R" {
    if (!num || $3!=torder || NF!=7 || $4!=0 || $5!~/^[01]$/) tool_fail("invalid selected reference")
    if (!(($5,$6) in active) || active[$5,$6]!=$7) tool_fail("PU reference differs from its active RPL")
    tool_count["selected_reference\t" tr "/L" $5 "[" $6 "]/d" ($7-tpoc)]++
}
$1=="U" {
    if (!use_header || $3!=torder || NF!=8 || $6<=0 || $7<=0) tool_fail("unmatched use event")
    if ($2=="CU") { uses++; used_area+=$6*$7; if ($8==0) inter++ }
    if ($2=="AMVR") amvr++
    if ($2=="SBT") sbts++
    sz=$6 "x" $7
    if ($2=="AMVR_signalled" || $2=="merge_index")
        tool_count["parameter_by_shape\t" $2 "/" tr "/" sz "/" $8]++
    if (($2=="SBT" && $8>0) || $2=="MMVD" || $2=="TS" || ($2=="CCLM" && $8>0))
        tool_count["block_tool\t" $2 "/" tr "/" sz "/" $8]++
}
END {
    if (broken) exit 2
    if (!num) tool_fail("no pictures")
    tool_finish()
    for (key in tool_count) print key "\t" tool_count[key]
}
