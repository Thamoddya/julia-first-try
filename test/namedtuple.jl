# This file is a part of Julia. License is MIT: https://julialang.org/license

@test_throws TypeError NamedTuple{1,Tuple{}}
@test_throws TypeError NamedTuple{(),1}
@test_throws TypeError NamedTuple{(:a,1),Tuple{Int}}
@test_throws ErrorException NamedTuple{(:a,:b),Tuple{Int}}
@test_throws ErrorException NamedTuple{(:a,:b),Tuple{Int,Vararg{Int}}}
@test_throws ErrorException NamedTuple{(:a,),Union{Tuple{Int},Tuple{String}}}
@test_throws ErrorException NamedTuple{(:a,:a),Tuple{Int,Int}}
@test_throws ErrorException NamedTuple{(:a,:a)}((1,2))
@test_throws ErrorException NamedTuple{(:a, :b, :a), NTuple{3, Int}}((1, 2, 3))
@test_throws ArgumentError NamedTuple{(:a, :b, :c), NTuple{3, Int}}((1, 2))

@test fieldcount(NamedTuple{(:a,:b,:c)}) == 3
@test fieldcount(NamedTuple{<:Any,Tuple{Int,Int}}) == 2
@test_throws ArgumentError fieldcount(NamedTuple)
@test_throws ArgumentError fieldcount(NamedTuple{<:Any,<:Tuple{Int,Vararg{Int}}})

@test (a=1,).a == 1
@test (a=2,)[1] == 2
@test (a=3,)[:a] == 3
@test (x=4, y=5, z=6).y == 5
@test (x=4, y=5, z=6).z == 6
@test (x=4, y=5, z=6)[(:x, :y)] == (x=4, y=5)
@test (x=4, y=5, z=6)[(:x,)] == (x=4,)
@test (x=4, y=5, z=6)[[:x, :y]] == (x=4, y=5)
@test (x=4, y=5, z=6)[[:x]] == (x=4,)
@test (x=4, y=5, z=6)[()] == NamedTuple()
@test (x=4, y=5, z=6)[:] == (x=4, y=5, z=6)
@test NamedTuple()[()] == NamedTuple()
@test_throws ErrorException (x=4, y=5, z=6).a
@test_throws BoundsError (a=2,)[0]
@test_throws BoundsError (a=2,)[2]
@test_throws ErrorException (x=4, y=5, z=6)[(:a,)]
@test_throws ErrorException (x=4, y=5, z=6)[(:x, :a)]
@test_throws ErrorException (x=4, y=5, z=6)[[:a]]
@test_throws ErrorException (x=4, y=5, z=6)[[:x, :a]]
@test_throws ErrorException (x=4, y=5, z=6)[(:x, :x)]

@test length(NamedTuple()) == 0
@test length((a=1,)) == 1
@test length((a=1, b=0)) == 2

@test firstindex((a=1, b=0)) == 1
@test firstindex((a=1,)) == 1
@test firstindex(NamedTuple()) == 1
@test lastindex((a=1, b=0)) == 2
@test lastindex((a=1,)) == 1
@test lastindex(NamedTuple()) == 0

@test isempty(NamedTuple())
@test !isempty((a=1,))
@test empty((a=1,)) === NamedTuple()
@test isempty(empty((a=1,)))

@test (a=1,b=2) === (a=1,b=2)
@test (a=1,b=2) !== (b=1,a=2)

@test (a=1,b=2) == (a=1,b=2)
@test (a=1,b=2) != (b=1,a=2)
@test NamedTuple() === NamedTuple()
@test NamedTuple() != (a=1,)
@test !isequal(NamedTuple(), (a=1,))

@test string((a=1,)) == "(a = 1,)"
@test string((name="", day=:today)) == "(name = \"\", day = :today)"
@test string(NamedTuple()) == "NamedTuple()"

@test hash((a = 1, b = "hello")) == hash(NamedTuple{(:a,:b),Tuple{Int,String}}((1, "hello")))
@test hash((a = 1, b = "hello")) != hash(NamedTuple{(:a,:c),Tuple{Int,String}}((1, "hello")))
@test hash((a = 1, b = "hello")) != hash(NamedTuple{(:a,:b),Tuple{Int,String}}((1, "helo")))

@test NamedTuple{(:a,:b),Tuple{Int8,Int16}}((1,2)) === (a=Int8(1), b=Int16(2))
@test convert(NamedTuple{(:a,:b),Tuple{Int8,Int16}}, (a=3,b=4)) === (a=Int8(3), b=Int16(4))
let NT = NamedTuple{(:a,:b),Tuple{Int8,Int16}}, nt = (x=3,y=4)
    @test_throws MethodError convert(NT, nt)
end

@testset "convert NamedTuple" begin
    conv1 = convert(NamedTuple{(:a,),Tuple{I}} where I, (;a=1))
    @test conv1 === (a = 1,)

    conv2 = convert(NamedTuple{(:a,),Tuple{Any}}, (;a=1))
    @test conv2 === NamedTuple{(:a,), Tuple{Any}}((1,))

    conv3 = convert(NamedTuple{(:a,),}, (;a=1))
    @test conv3 === (a = 1,)

    conv4 = convert(NamedTuple{(:a,),Tuple{I}} where I<:Unsigned, (;a=1))
    @test conv4 === NamedTuple{(:a,), Tuple{Unsigned}}((1,))

    conv5 = convert(NamedTuple, (;a=1))
    @test conv1 === (a = 1,)

    conv_res = @test_throws MethodError convert(NamedTuple{(:a,),Tuple{I}} where I<:AbstractString, (;a=1))
    @test conv_res.value.f === convert && conv_res.value.args === (AbstractString, 1)

    conv6 = convert(NamedTuple{(:a,),Tuple{NamedTuple{(:b,), Tuple{Int}}}}, ((1,),))
    @test conv6 === (a = (b = 1,),)
end

@test NamedTuple{(:a,:c)}((b=1,z=2,c=3,aa=4,a=5)) === (a=5, c=3)
@test NamedTuple{(:a,)}(NamedTuple{(:b, :a), Tuple{Int, Union{Int,Nothing}}}((1, 2))) ===
    NamedTuple{(:a,), Tuple{Union{Int,Nothing}}}((2,))

@test eltype((a=[1,2], b=[3,4])) === Vector{Int}
@test eltype(NamedTuple{(:x, :y),Tuple{Union{Missing, Int},Union{Missing, Float64}}}(
    (missing, missing))) === Union{Real, Missing}

@test valtype((a=[1,2], b=[3,4])) === Vector{Int}
@test keytype((a=[1,2], b=[3,4])) === Symbol

@test Tuple((a=[1,2], b=[3,4])) == ([1,2], [3,4])
@test Tuple(NamedTuple()) === ()
@test Tuple((x=4, y=5, z=6)) == (4,5,6)
@test collect((x=4, y=5, z=6)) == [4,5,6]
@test Tuple((a=1, b=2, c=3)) == (1, 2, 3)

@test isless((a=1,b=2), (a=1,b=3))
@test_throws MethodError isless((a=1,), (a=1,b=2))
@test !isless((a=1,b=2), (a=1,b=2))
@test !isless((a=2,b=1), (a=1,b=2))
@test_throws MethodError isless((a=1,), (x=2,))

@test (a=1,b=2) < (a=1,b=3)
@test_throws MethodError (a=1,) < (a=1,b=2)
@test !((a=1,b=2) < (a=1,b=2))
@test !((a=2,b=1) < (a=1,b=2))
@test_throws MethodError (a=1,) < (x=2,)
@test !((a=-0.0,) < (a=0.0,))
@test ismissing((a=missing,) < (a=1,))
@test ismissing((a=missing,) < (a=missing,))

@test map(-, (x=1, y=2)) == (x=-1, y=-2)
@test map(+, (x=1, y=2), (x=10, y=20)) == (x=11, y=22)
@test_throws ArgumentError map(+, (x=1, y=2), (y=10, x=20))
@test map(string, (x=1, y=2)) == (x="1", y="2")
@test map(round, (x=UInt, y=Int), (x=3.1, y=2//3)) == (x=UInt(3), y=1)

@testset "filter" begin
    @test filter(isodd, (a=1,b=2,c=3)) === (a=1, c=3)
    @test filter(i -> true, (;)) === (;)
    longnt = NamedTuple{ntuple(i -> Symbol(:a, i), 20)}(ntuple(identity, 20))
    @test filter(iseven, longnt) === NamedTuple{ntuple(i -> Symbol(:a, 2i), 10)}(ntuple(i -> 2i, 10))
    @test filter(x -> x<2, (longnt..., z=1.5)) === (a1=1, z=1.5)
end

@test merge((a=1, b=2), (a=10,)) == (a=10, b=2)
@test merge((a=1, b=2), (a=10, z=20)) == (a=10, b=2, z=20)
@test merge((a=1, b=2), (z=20,)) == (a=1, b=2, z=20)
@test merge(NamedTuple(), (a=2, b=1)) == (a=2, b=1)
@test merge((a=2, b=1), NamedTuple()) == (a=2, b=1)
@test merge(NamedTuple(), NamedTuple()) == NamedTuple()
# `merge` should preserve element types
let nt = merge(NamedTuple{(:a,:b),Tuple{Int32,Union{Int32,Nothing}}}((1,Int32(2))),
               NamedTuple{(:a,:c),Tuple{Union{Int8,Nothing},Float64}}((nothing,1.0)))
    @test typeof(nt) == NamedTuple{(:a,:b,:c),Tuple{Union{Int8,Nothing},Union{Int32,Nothing},Float64}}
    @test repr(nt) == "@NamedTuple{a::Union{Nothing, Int8}, b::Union{Nothing, Int32}, c::Float64}((nothing, 2, 1.0))"
end

@test merge(NamedTuple(), [:a=>1, :b=>2, :c=>3, :a=>4, :c=>5]) == (a=4, b=2, c=5)
@test merge((c=0, z=1), [:a=>1, :b=>2, :c=>3, :a=>4, :c=>5]) == (c=5, z=1, a=4, b=2)

@test keys((a=1, b=2, c=3)) == (:a, :b, :c)
@test keys(NamedTuple()) == ()
@test keys((a=1,)) == (:a,)
@test values((a=1, b=2, c=3)) == (1, 2, 3)
@test values(NamedTuple()) == ()
@test values((a=1,)) == (1,)
@test haskey((a=1, b=2, c=3), :a)
@test !haskey(NamedTuple(), :a)
@test !haskey((a=1,), :b)
@test get((a=1, b=2, c=3), :a, 0) == 1
@test get(NamedTuple(), :a, 0) == 0
@test get((a=1,), :b, 0) == 0
@test get(()->0, (a=1, b=2, c=3), :a) == 1
@test get(()->0, NamedTuple(), :a) == 0
@test get(()->0, (a=1,), :b) == 0
@test Base.tail((a = 1, b = 2.0, c = 'x')) ≡ (b = 2.0, c = 'x')
@test Base.tail((a = 1, )) ≡ NamedTuple()
@test Base.front((a = 1, b = 2.0, c = 'x')) ≡ (a = 1, b = 2.0)
@test Base.front((a = 1, )) ≡ NamedTuple()
@test_throws ArgumentError Base.tail(NamedTuple())
@test_throws ArgumentError Base.front(NamedTuple())
@test @inferred(reverse((a=1,))) === (a=1,)
@test @inferred(reverse((a=1, b=:c))) === (b=:c, a=1)

# syntax errors

@test Meta.lower(Main, Meta.parse("(a=1, 0)")) == Expr(:error, "invalid named tuple element \"0\"")
@test Meta.lower(Main, Meta.parse("(a=1, f(x))")) == Expr(:error, "invalid named tuple element \"f(x)\"")
@test Meta.lower(Main, Meta.parse("(a=1,a=2)")) == Expr(:error, "field name \"a\" repeated in named tuple")
@test Meta.lower(Main, Meta.parse("(a=1,b=0,a=2)")) == Expr(:error, "field name \"a\" repeated in named tuple")
@test Meta.lower(Main, Meta.parse("(c=1,a=1,b=0,a=2)")) == Expr(:error, "field name \"a\" repeated in named tuple")

@test Meta.lower(Main, Meta.parse("(; f(x))")) == Expr(:error, "invalid named tuple element \"f(x)\"")
@test Meta.lower(Main, Meta.parse("(;1=0)")) == Expr(:error, "invalid named tuple field name \"1\"")

@test eval(Expr(:tuple, Expr(:parameters))) === NamedTuple()
@test Meta.lower(Main, Meta.parse("(1,;2)")) == Expr(:error, "unexpected semicolon in tuple")

# splatting

let d = [:a=>1, :b=>2, :c=>3]   # use an array to preserve order
    @test (d..., a=10) == (a=10, b=2, c=3)
    @test (a=0, b=0, z=1, d..., x=4, y=5) == (a=1, b=2, z=1, c=3, x=4, y=5)
    @test (a=0, (b=2,a=1)..., c=3) == (a=1, b=2, c=3)

    t = (x=1, y=20)
    @test (;d...) == (a=1, b=2, c=3)
    @test (;d..., :z=>20) == (a=1, b=2, c=3, z=20)
    @test (;a=10, d..., :c=>30) == (a=1, b=2, c=30)
    y = (w=30, z=40)
    @test (;t..., y...) == (x=1, y=20, w=30, z=40)
    @test (;t..., y=0, y...) == (x=1, y=0, w=30, z=40)

    @test NamedTuple(d) === (a=1, b=2, c=3)
end

# inference tests

namedtuple_get_a(x) = x.a
@test Base.return_types(namedtuple_get_a, (NamedTuple,)) == Any[Any]
@test Base.return_types(namedtuple_get_a, (typeof((b=1,a="")),)) == Any[String]

namedtuple_fieldtype_a(x) = fieldtype(typeof(x), :a)
@test Base.return_types(namedtuple_fieldtype_a, (NamedTuple,)) == Any[Union{Type, TypeVar}]
@test Base.return_types(namedtuple_fieldtype_a, (typeof((b=1,a="")),)) == Any[Type{String}]
namedtuple_fieldtype__(x, y) = fieldtype(typeof(x), y)
@test Base.return_types(namedtuple_fieldtype__, (typeof((b=1,a="")),Symbol))[1] >: Union{Type{Int}, Type{String}}

namedtuple_nfields(x) = nfields(x) === 0 ? 1 : ""
@test Union{Int,String} <: Base.return_types(namedtuple_nfields, (NamedTuple,))[1]

function nt_from_abstractly_typed_array()
    a = NamedTuple[(a=3,b=5)]
    (getfield(a[1],1), getfield(a[1],2))
end
@test nt_from_abstractly_typed_array() === (3,5)

let T = NamedTuple{(:a, :b), Tuple{Int64, Union{Float64, Nothing}}}, nt = T((1, nothing))
    @test nt == (a=1, b=nothing)
    @test typeof(nt) == T
    @test convert(T, (a=1, b=nothing)) == nt
    @test typeof(convert(T, (a=1, b=nothing))) === T
end

function abstr_nt_22194()
    a = NamedTuple[(a=1,), (b=2,)]
    return (a[1].a, a[2].b)
end
@test abstr_nt_22194() == (1, 2)
@test Base.return_types(abstr_nt_22194, ()) == Any[Tuple{Any,Any}]
function abstr_nt_22194_2()
    a = NamedTuple[(a=1,), (b=2,)]
    return a[1].b
end
@test_throws ErrorException abstr_nt_22194_2()
@test Base.return_types(abstr_nt_22194_2, ()) == Any[Any]

mutable struct HasAbstractNamedTuples
    x::NamedTuple{(:a,:b)}
end

function abstr_nt_22194_3()
    s = HasAbstractNamedTuples((a="",b=8))
    @test s.x.a === ""
    @test s.x.b === 8
    s.x = (a=1,b=:b)
    @test s.x.a === 1
    @test s.x.b === :b
    @test isdefined(s.x, :a)
    @test isdefined(s.x, :b)
    @test !isdefined(s.x, :c)
    @test nfields(s) == 1
    @test isdefined(s, :x)
    @test fieldtype(typeof(s), 1) == fieldtype(typeof(s), :x) == NamedTuple{(:a,:b)}
    @test fieldtype(typeof(s.x), :a) === Int
    @test fieldtype(typeof(s.x), :b) === Symbol
    return s.x.b
end
abstr_nt_22194_3()
@test Base.return_types(abstr_nt_22194_3, ()) == Any[Any]

@test Base.structdiff((a=1, b=2), (b=3,)) == (a=1,)
@test Base.structdiff((a=1, b=2, z=20), (b=3,)) == (a=1, z=20)
@test Base.structdiff((a=1, b=2, z=20), (b=3, q=20, z=1)) == (a=1,)
@test Base.structdiff((a=1, b=2, z=20), (b=3, q=20, z=1, a=0)) == NamedTuple()
@test Base.structdiff((a=1, b=2, z=20), NamedTuple{(:b,)}) == (a=1, z=20)
@test typeof(Base.structdiff(NamedTuple{(:a, :b), Tuple{Int32, Union{Int32, Nothing}}}((1, Int32(2))),
                             (a=0,))) === NamedTuple{(:b,), Tuple{Union{Int32, Nothing}}}

@test findall(isequal(1), (a=1, b=2)) == [:a]
@test findall(isequal(1), (a=1, b=1)) == [:a, :b]
@test isempty(findall(isequal(1), NamedTuple()))
@test isempty(findall(isequal(1), (a=2, b=3)))
@test findfirst(isequal(1), (a=1, b=2)) === :a
@test findlast(isequal(1), (a=1, b=2)) === :a
@test findfirst(isequal(1), (a=1, b=1)) === :a
@test findlast(isequal(1), (a=1, b=1)) === :b
@test findfirst(isequal(1), ()) === nothing
@test findlast(isequal(1), ()) === nothing
@test findfirst(isequal(1), (a=2, b=3)) === nothing
@test findlast(isequal(1), (a=2, b=3)) === nothing

# Test map with Nothing and Missing
for T in (Nothing, Missing)
    x = [(a=1, b=T()), (a=1, b=2)]
    y = map(v -> (a=v.a, b=v.b), [(a=1, b=T()), (a=1, b=2)])
    @test y isa Vector{NamedTuple{(:a,:b), T} where T<:Tuple}
    @test isequal(x, y)
end
y = map(v -> (a=v.a, b=v.a + v.b), [(a=1, b=missing), (a=1, b=2)])
@test y isa Vector{NamedTuple{(:a,:b), T} where T<:Tuple}
@test isequal(y, [(a=1, b=missing), (a=1, b=3)])

# issue #27187
@test reduce(merge,[(a = 1, b = 2), (c = 3, d = 4)]) == (a = 1, b = 2, c = 3, d = 4)
@test typeintersect(NamedTuple{()}, NamedTuple{names, Tuple{Int,Int}} where names) == Union{}

# Iterator constructor
@test NamedTuple{(:a, :b), Tuple{Int, Float64}}(Any[1.0, 2]) === (a=1, b=2.0)
@test NamedTuple{(:a, :b)}(Any[1.0, 2]) === (a=1.0, b=2)

# Left-associative merge, issue #29215
@test merge((a=1, b=2), (b=3, c=4), (c=5,)) === (a=1, b=3, c=5)
@test merge((a=1, b=2), (b=3, c=(d=1,)), (c=(d=2,),)) === (a=1, b=3, c=(d=2,))
@test merge((a=1, b=2)) === (a=1, b=2)

# issue #33270
let n = NamedTuple{(:T,), Tuple{Type{Float64}}}((Float64,))
    @test n isa NamedTuple{(:T,), Tuple{Type{Float64}}}
    @test n.T === Float64
end

# setindex
let nt0 = NamedTuple(), nt1 = (a=33,), nt2 = (a=0, b=:v)
    @test Base.setindex(nt0, 33, :a) == nt1
    @test Base.setindex(Base.setindex(nt1, 0, :a), :v, :b) == nt2
    @test Base.setindex(nt1, "value", :a) == (a="value",)
    @test Base.setindex(nt1, "value", :a) isa NamedTuple{(:a,),<:Tuple{AbstractString}}
end

# @NamedTuple
@testset "@NamedTuple" begin
    @test @NamedTuple{a::Int, b::String} === NamedTuple{(:a, :b),Tuple{Int,String}} ===
        @NamedTuple begin
            a::Int
            b::String
        end
    @test @NamedTuple{a::Int, b} === NamedTuple{(:a, :b),Tuple{Int,Any}}
    @test_throws LoadError include_string(Main, "@NamedTuple{a::Int, b, 3}")
    @test_throws LoadError include_string(Main, "@NamedTuple(a::Int, b)")
end

# @Kwargs
@testset "@Kwargs" begin
   @test @Kwargs{a::Int,b::String}  == typeof(pairs((;a=1,b="2")))
   @test @Kwargs{} == typeof(pairs((;)))
end

# issue #29333, implicit names
let x = 1, y = 2
    @test (;y) === (y = 2,)
    a = (; x, y)
    @test a === (x=1, y=2)
    @test (; a.y, a.x) === (y=2, x=1)
    y = 3
    @test Meta.lower(Main, Meta.parse("(; a.y, y)")) == Expr(:error, "field name \"y\" repeated in named tuple")
    @test (; a.y, x) === (y=2, x=1)
end

# issue #37926
@test nextind((a=1,), 1) == nextind((1,), 1) == 2
@test prevind((a=1,), 2) == prevind((1,), 2) == 1

# issue #43045
@test merge(NamedTuple(), Iterators.reverse(pairs((a=1,b=2)))) === (b = 2, a = 1)

# issue #44086
@test NamedTuple{(:x, :y, :z), Tuple{Int8, Int16, Int32}}((z=1, x=2, y=3)) === (x = Int8(2), y = Int16(3), z = Int32(1))

@testset "mapfoldl" begin
    A1 = (;a=1, b=2, c=3, d=4)
    A2 = (;a=-1, b=-2, c=-3, d=-4)
    @test (((1=>2)=>3)=>4) == foldl(=>, A1) ==
          mapfoldl(identity, =>, A1) == mapfoldl(abs, =>, A2)
    @test mapfoldl(abs, =>, A2, init=-10) == ((((-10=>1)=>2)=>3)=>4)
    @test mapfoldl(abs, =>, (;), init=-10) == -10
    @test mapfoldl(abs, Pair{Any,Any}, NamedTuple(Symbol(:x,i) => i for i in 1:30)) == mapfoldl(abs, Pair{Any,Any}, [1:30;])
    @test_throws "reducing over an empty collection" mapfoldl(abs, =>, (;))
end

# Test effect/inference for merge/diff of unknown NamedTuples
for f in (Base.merge, Base.structdiff)
    @testset let f = f
        # test the effects of the fallback path
        fallback_func(a::NamedTuple, b::NamedTuple) = @invoke f(a::NamedTuple, b::NamedTuple)
        @testset let eff = Base.infer_effects(fallback_func)
            @test Core.Compiler.is_foldable(eff)
            @test Core.Compiler.is_nonoverlayed(eff)
        end
        @test only(Base.return_types(fallback_func)) == NamedTuple
        # test if `max_methods = 4` setting works as expected
        general_func(a::NamedTuple, b::NamedTuple) = f(a, b)
        @testset let eff = Base.infer_effects(general_func)
            @test Core.Compiler.is_foldable(eff)
            @test Core.Compiler.is_nonoverlayed(eff)
        end
        @test only(Base.return_types(general_func)) == NamedTuple
    end
end
@test Core.Compiler.is_foldable(Base.infer_effects(pairs, Tuple{NamedTuple}))

# Test that merge/diff preserves nt field types
let a = Base.NamedTuple{(:a, :b), Tuple{Any, Any}}((1, 2)), b = Base.NamedTuple{(:b,), Tuple{Float64}}(3)
    @test typeof(Base.merge(a, b)) == Base.NamedTuple{(:a, :b), Tuple{Any, Float64}}
    @test typeof(Base.structdiff(a, b)) == Base.NamedTuple{(:a,), Tuple{Any}}
end

function mergewith51009(combine, a::NamedTuple{an}, b::NamedTuple{bn}) where {an, bn}
    names = Base.merge_names(an, bn)
    NamedTuple{names}(ntuple(Val{nfields(names)}()) do i
                          n = getfield(names, i)
                          if Base.sym_in(n, an)
                              if Base.sym_in(n, bn)
                                  combine(getfield(a, n), getfield(b, n))
                              else
                                  getfield(a, n)
                              end
                          else
                              getfield(b, n)
                          end
                      end)
end
let c = (a=1, b=2),
    d = (b=3, c=(d=1,))
    @test @inferred(mergewith51009((x,y)->y, c, d)) === (a = 1, b = 3, c = (d = 1,))
end

@test_throws ErrorException NamedTuple{(), Union{}}
for NT in (NamedTuple{(:a, :b), Union{}}, NamedTuple{(:a, :b), T} where T<:Union{})
    @test fieldtype(NT, 1) == Union{}
    @test fieldtype(NT, :b) == Union{}
    @test_throws ErrorException fieldtype(NT, :c)
    @test_throws BoundsError fieldtype(NT, 0)
    @test_throws BoundsError fieldtype(NT, 3)
    @test Base.return_types((Type{NT},)) do NT; fieldtype(NT, :a); end == Any[Type{Union{}}]
    @test fieldtype(NamedTuple{<:Any, Union{}}, 1) == Union{}
end
let NT = NamedTuple{<:Any, Union{}}
    @test fieldtype(NT, 100) == Union{}
    @test only(Base.return_types((Type{NT},)) do NT; fieldtype(NT, 100); end) >: Type{Union{}}
end
