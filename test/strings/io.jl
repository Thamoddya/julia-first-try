# This file is a part of Julia. License is MIT: https://julialang.org/license

@testset "string escaping & unescaping" begin
    cx = Any[
        0x00000000      '\0'        "\\0"
        0x00000001      '\x01'      "\\x01"
        0x00000006      '\x06'      "\\x06"
        0x00000007      '\a'        "\\a"
        0x00000008      '\b'        "\\b"
        0x00000009      '\t'        "\\t"
        0x0000000a      '\n'        "\\n"
        0x0000000b      '\v'        "\\v"
        0x0000000c      '\f'        "\\f"
        0x0000000d      '\r'        "\\r"
        0x0000000e      '\x0e'      "\\x0e"
        0x0000001a      '\x1a'      "\\x1a"
        0x0000001b      '\e'        "\\e"
        0x0000001c      '\x1c'      "\\x1c"
        0x0000001f      '\x1f'      "\\x1f"
        0x00000020      ' '         " "
        0x00000022      '"'         "\\\""
        0x0000002f      '/'         "/"
        0x00000030      '0'         "0"
        0x00000039      '9'         "9"
        0x0000003a      ':'         ":"
        0x00000040      '@'         "@"
        0x00000041      'A'         "A"
        0x0000005a      'Z'         "Z"
        0x0000005b      '['         "["
        0x0000005c      '\\'         "\\\\"
        0x00000060      '`'         "`"
        0x00000061      'a'         "a"
        0x0000007a      'z'         "z"
        0x0000007b      '{'         "{"
        0x0000007e      '~'         "~"
        0x0000007f      '\x7f'      "\\x7f"
        0x000000bf      '\ubf'      "\\ubf"
        0x000000ff      '\uff'      "\\uff"
        0x00000100      '\u100'     "\\u100"
        0x000001ff      '\u1ff'     "\\u1ff"
        0x00000fff      '\ufff'     "\\ufff"
        0x00001000      '\u1000'    "\\u1000"
        0x00001fff      '\u1fff'    "\\u1fff"
        0x0000ffff      '\uffff'    "\\uffff"
        0x00010000      '\U10000'   "\\U10000"
        0x0001ffff      '\U1ffff'   "\\U1ffff"
        0x0002ffff      '\U2ffff'   "\\U2ffff"
        0x00030000      '\U30000'   "\\U30000"
        0x000dffff      '\Udffff'   "\\Udffff"
        0x000e0000      '\Ue0000'   "\\Ue0000"
        0x000effff      '\Ueffff'   "\\Ueffff"
        0x000f0000      '\Uf0000'   "\\Uf0000"
        0x000fffff      '\Ufffff'   "\\Ufffff"
        0x00100000      '\U100000'  "\\U100000"
        0x0010ffff      '\U10ffff'  "\\U10ffff"
    ]

    buf = IOBuffer()
    @test typeof(escape_string(buf, "test")) == Nothing
    @test String(take!(buf)) == "test"
    @test typeof(escape_string(buf, "hello", "l")) == Nothing
    @test String(take!(buf)) == "he\\l\\lo"

    @test typeof(escape_string("test", "t")) == String
    @test escape_string("test", "t") == "\\tes\\t"

    @test escape_string("\\cdot") == "\\\\cdot"
    @test escape_string("\\cdot"; keep = '\\') == "\\cdot"
    @test escape_string("\\cdot", '\\'; keep = '\\') == "\\\\cdot"
    @test escape_string("\\cdot\n"; keep = "\\\n") == "\\cdot\n"
    @test escape_string("\\cdot\n", '\n'; keep = "\\\n") == "\\cdot\\\n"
    @test escape_string("\\cdot\n", "\\\n"; keep = "\\\n") == "\\\\cdot\\\n"

    for i = 1:size(cx,1)
        cp, ch, st = cx[i,:]
        @test cp == convert(UInt32, ch)
        @test string(ch) == unescape_string(st)
        if isascii(ch) || !isprint(ch)
            @test st == escape_string(string(ch))
        end
        for j = 1:size(cx,1)
            local str = string(ch, cx[j,2])
            @test str == unescape_string(escape_string(str))
        end
        @test repr(ch) == "'$(isprint(ch) && ch != '\\' ? ch : st)'"
    end

    for i = 0:0x7f, p = ["","\0","x","xxx","\x7f","\uFF","\uFFF",
                         "\uFFFF","\U10000","\U10FFF","\U10FFFF"]
        c = Char(i)
        cp = string(c,p)
        op = string(Char(div(i,8)), string(i%8, base = 8), p)
        hp = string(Char(div(i,16)), string(i%16, base = 16), p)
        @test string(unescape_string(string("\\",string(i,base=8,pad=1),p))) == cp
        @test string(unescape_string(string("\\",string(i,base=8,pad=2),p))) == cp
        @test string(unescape_string(string("\\",string(i,base=8,pad=3),p))) == cp
        @test string(unescape_string(string("\\",string(i,base=8,pad=4),p))) == op
        @test string(unescape_string(string("\\x",string(i,base=16,pad=1),p))) == cp
        @test string(unescape_string(string("\\x",string(i,base=16,pad=2),p))) == cp
        @test string(unescape_string(string("\\x",string(i,base=16,pad=3),p))) == hp
    end

    @testset "unescape_string" begin
        @test "\0" == unescape_string("\\0")
        @test "\1" == unescape_string("\\1")
        @test "\7" == unescape_string("\\7")
        @test "\0x" == unescape_string("\\0x")
        @test "\1x" == unescape_string("\\1x")
        @test "\7x" == unescape_string("\\7x")
        @test "\00" == unescape_string("\\00")
        @test "\01" == unescape_string("\\01")
        @test "\07" == unescape_string("\\07")
        @test "\70" == unescape_string("\\70")
        @test "\71" == unescape_string("\\71")
        @test "\77" == unescape_string("\\77")
        @test "\00x" == unescape_string("\\00x")
        @test "\01x" == unescape_string("\\01x")
        @test "\07x" == unescape_string("\\07x")
        @test "\70x" == unescape_string("\\70x")
        @test "\71x" == unescape_string("\\71x")
        @test "\77x" == unescape_string("\\77x")
        @test "\000" == unescape_string("\\000")
        @test "\001" == unescape_string("\\001")
        @test "\007" == unescape_string("\\007")
        @test "\070" == unescape_string("\\070")
        @test "\071" == unescape_string("\\071")
        @test "\077" == unescape_string("\\077")
        @test "\170" == unescape_string("\\170")
        @test "\171" == unescape_string("\\171")
        @test "\177" == unescape_string("\\177")
        @test "\0001" == unescape_string("\\0001")
        @test "\0011" == unescape_string("\\0011")
        @test "\0071" == unescape_string("\\0071")
        @test "\0701" == unescape_string("\\0701")
        @test "\0711" == unescape_string("\\0711")
        @test "\0771" == unescape_string("\\0771")
        @test "\1701" == unescape_string("\\1701")
        @test "\1711" == unescape_string("\\1711")
        @test "\1771" == unescape_string("\\1771")

        @test "\x0" == unescape_string("\\x0")
        @test "\x1" == unescape_string("\\x1")
        @test "\xf" == unescape_string("\\xf")
        @test "\xF" == unescape_string("\\xF")
        @test "\x0x" == unescape_string("\\x0x")
        @test "\x1x" == unescape_string("\\x1x")
        @test "\xfx" == unescape_string("\\xfx")
        @test "\xFx" == unescape_string("\\xFx")
        @test "\x00" == unescape_string("\\x00")
        @test "\x01" == unescape_string("\\x01")
        @test "\x0f" == unescape_string("\\x0f")
        @test "\x0F" == unescape_string("\\x0F")

        str= "aaa \\g \\n"
        @test "aaa \\g \n" == unescape_string(str, ['g'])
        @test "aaa \\g \\n" == unescape_string(str, ['g', 'n'])
    end
    @test Base.escape_raw_string(raw"\"\\\"\\-\\") == "\\\"\\\\\\\"\\\\-\\\\"
    @test Base.escape_raw_string(raw"`\`\\-\\") == "\`\\\`\\\\-\\\\"
    @test Base.escape_raw_string(raw"\"\\\"\\-\\", '`') == "\"\\\"\\\\-\\\\"
    @test Base.escape_raw_string(raw"`\`\\-\\", '`') == "\\\`\\\\\\\`\\\\-\\\\"
    @test Base.escape_raw_string(raw"some`string") == "some`string"
    @test Base.escape_raw_string(raw"some\"string", '`') == "some\"string"
    @test Base.escape_raw_string(raw"some`string\\") == "some`string\\\\"
    @test Base.escape_raw_string(raw"some\"string\\", '`') == "some\"string\\\\"
    @test Base.escape_raw_string(raw"some\"string") == "some\\\"string"
    @test Base.escape_raw_string(raw"some`string", '`') == "some\\`string"
end
@testset "join()" begin
    @test join([]) == join([],",") == ""
    @test join(()) == join((),",") == ""
    @test join(["a"],"?") == "a"
    @test join("HELLO",'-') == "H-E-L-L-O"
    @test join(1:5, ", ", " and ") == "1, 2, 3, 4 and 5"
    @test join(["apples", "bananas", "pineapples"], ", ", " and ") == "apples, bananas and pineapples"
    @test_throws MethodError join(1, 2, 3, 4)
    @test join(()) == ""
    @test join((), ", ") == ""
    @test join((), ", ", ", and ") == ""
end

# quotes + interpolation (issue #455)
@test "$("string")" == "string"
arr = ["a","b","c"]
@test "[$(join(arr, " - "))]" == "[a - b - c]"

# join with empty input
myio = IOBuffer()
join(myio, "", "", 1)
@test isempty(take!(myio))

@testset "unescape_string ArgumentErrors" begin
    @test_throws ArgumentError unescape_string(IOBuffer(), string('\\',"xZ"))
    @test_throws ArgumentError unescape_string(IOBuffer(), string('\\',"777"))
    @test_throws ArgumentError unescape_string(IOBuffer(), string('\\',"U110000"))
    @test_throws ArgumentError unescape_string(IOBuffer(), string('\\',"N"))
    @test_throws ArgumentError unescape_string(IOBuffer(), string('\\',"m"))
end

@testset "sprint with context" begin
    function f(io::IO)
        println(io, "compact => ", get(io, :compact, false)::Bool)
        println(io, "limit   => ", get(io, :limit,   false)::Bool)
    end

    str = sprint(f)
    @test str == """
        compact => false
        limit   => false
        """

    str = sprint(f, context = :compact => true)
    @test str == """
        compact => true
        limit   => false
        """

    str = sprint(f, context = (:compact => true, :limit => true))
    @test str == """
        compact => true
        limit   => true
        """

    str = sprint(f, context = IOContext(stdout, :compact => true, :limit => true))
    @test str == """
        compact => true
        limit   => true
        """
end

@testset "sprint honoring IOContext" begin
    @test startswith(sprint(show, Base.Dict[], context=(:compact=>false, :module=>nothing)), "Base.Dict")
end

@testset "#11659" begin
    # The indentation code was not correctly counting tab stops
    @test Base.indentation("      \t") == (8, true)
    @test Base.indentation("  \tfoob") == (8, false)
    @test Base.indentation(" \t \t")   == (16, true)

    @test Base.unindent("\tfoo",0) == "\tfoo"
    @test Base.unindent("\tfoo",4) == "    foo"
    @test Base.unindent("    \tfoo",4) == "    foo"
    @test Base.unindent("\t\n    \tfoo",4) == "    \n    foo"
    @test Base.unindent("\tfoo\tbar",4) == "    foo     bar"
    @test Base.unindent("\n\tfoo",4) == "\n    foo"
    @test Base.unindent("\n    \tfoo",4) == "\n    foo"
    @test Base.unindent("\n\t\n    \tfoo",4) == "\n    \n    foo"
    @test Base.unindent("\n\tfoo\tbar",4) == "\n    foo     bar"
end

# issue #22021, string realloc bug with join
s22021 = String["\"\"\"
     non_max_suppression(boxes, scores, max_output_size; iou_threshold=nothing)

Greedily selects a subset of bounding boxes in descending order of score,

pruning away boxes that have high intersection-over-union (IOU) overlap
with previously selected boxes.  Bounding boxes are supplied as
[y1, x1, y2, x2], where (y1, x1) and (y2, x2) are the coordinates of any
diagonal pair of box corners and the coordinates can be provided as normalized
(i.e., lying in the interval [0, 1]) or absolute.  Note that this algorithm
is agnostic to where the origin is in the coordinate system.  Note that this
algorithm is invariant to orthogonal transformations and translations
of the coordinate system; thus translating or reflections of the coordinate
system result in the same boxes being selected by the algorithm.

The output of this operation is a set of integers indexing into the input
collection of bounding boxes representing the selected boxes.  The bounding
box coordinates corresponding to the selected indices can then be obtained
using the `tf.gather operation`.  For example:

  selected_indices = tf.image.non_max_suppression(
      boxes, scores, max_output_size, iou_threshold)
  selected_boxes = tf.gather(boxes, selected_indices)
\"\"\"",
    "    tf.@op function non_max_suppression(v13566, v13567, v13568; name=nothing, iou_threshold=nothing) ",
    "            local desc ",
    "            tf.with_op_name((()->begin  ",
    "                        desc = tf.NodeDescription(\"NonMaxSuppression\") ",
    "                        begin  ",
    "                            begin  ",
    "                                v13566 = convert(TensorFlow.Tensor{Float32}, v13566) ",
    "                                begin  ",
    "                                end",
    "                            end",
    "                            begin  ",
    "                                v13567 = convert(TensorFlow.Tensor{Float32}, v13567) ",
    "                                begin  ",
    "                                end",
    "                            end",
    "                            begin  ",
    "                                v13568 = convert(TensorFlow.Tensor{Int32}, v13568) ",
    "                                begin  ",
    "                                end",
    "                            end",
    "                        end ",
    "                        begin  ",
    "                            begin  ",
    "                                tf.add_input(desc, v13566)",
    "                            end",
    "                            begin  ",
    "                                tf.add_input(desc, v13567)",
    "                            end",
    "                            begin  ",
    "                                tf.add_input(desc, v13568)",
    "                            end",
    "                        end ",
    "                        begin  ",
    "                            begin  ",
    "                                if iou_threshold !== nothing ",
    "                                    desc[\"iou_threshold\"] = Base.identity(iou_threshold)",
    "                                end",
    "                            end",
    "                        end",
    "                    end), name, \"NonMaxSuppression\") ",
    "            tf.Tensor(tf.Operation(desc))",
    "        end"]

for i = 1:10
    buf = IOBuffer()
    print(buf, join(s22021, "\n"))
    @test isvalid(String, take!(buf))
end

@testset "string()" begin
    # test the Float sizehints
    @test string(2.f0) == "2.0"
    @test string(2.f0, 2.0) == "2.02.0"
    # test empty args
    @test string() == ""
end

module StringsIOStringReturnTypesTestModule
    struct S end
    Base.joinpath(::S) = S()
end

@testset "`string` return types" begin
    @test all(T -> T <: AbstractString, Base.return_types(string))
end

@testset "type stable `join` (#55389)" begin
    itr = ("foo" for _ in 1:100)
    @test Base.return_types(join, (typeof(itr),))[] == String
end
