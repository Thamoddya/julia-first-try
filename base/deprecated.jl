# This file is a part of Julia. License is MIT: https://julialang.org/license

# Deprecated functions and objects
#
# Please add new deprecations at the bottom of the file.
# A function deprecated in a release will be removed in the next one.
# Please also add a reference to the pull request which introduced the
# deprecation. For simple cases where a direct replacement is available,
# use @deprecate. @deprecate takes care of calling the replacement
# and of exporting the function.
#
# For more complex cases, move the body of the deprecated method in this file,
# and call depwarn() directly from inside it.

"""
    @deprecate old new [export_old=true]

Deprecate method `old` and specify the replacement call `new`, defining a new method `old`
with the specified signature in the process.

To prevent `old` from being exported, set `export_old` to `false`.

See also [`Base.depwarn()`](@ref).

!!! compat "Julia 1.5"
    As of Julia 1.5, functions defined by `@deprecate` do not print warning when `julia`
    is run without the `--depwarn=yes` flag set, as the default value of `--depwarn` option
    is `no`.  The warnings are printed from tests run by `Pkg.test()`.

# Examples
```jldoctest
julia> @deprecate old(x) new(x)
old (generic function with 1 method)

julia> @deprecate old(x) new(x) false
old (generic function with 1 method)
```

Calls to `@deprecate` without explicit type-annotations will define
deprecated methods accepting any number of positional and keyword
arguments of type `Any`.

!!! compat "Julia 1.9"
    Keyword arguments are forwarded when there is no explicit type
    annotation as of Julia 1.9. For older versions, you can manually
    forward positional and keyword arguments by doing `@deprecate
    old(args...; kwargs...) new(args...; kwargs...)`.

To restrict deprecation to a specific signature, annotate the
arguments of `old`. For example,
```jldoctest; filter = r"@ .*"a
julia> new(x::Int) = x;

julia> new(x::Float64) = 2x;

julia> @deprecate old(x::Int) new(x);

julia> methods(old)
# 1 method for generic function "old" from Main:
 [1] old(x::Int64)
     @ deprecated.jl:94
```
will define and deprecate a method `old(x::Int)` that mirrors `new(x::Int)` but will not
define nor deprecate the method `old(x::Float64)`.
"""
macro deprecate(old, new, export_old=true)
    function cannot_export_nonsymbol()
        error(
            "if the third `export_old` argument is not specified or `true`, the first",
            " argument must be of form",
            " (1) `f(...)` where `f` is a symbol,",
            " (2) `T{...}(...)` where `T` is a symbol, or",
            " (3) a symbol.",
        )
    end
    meta = Expr(:meta, :noinline)
    if isa(old, Expr) && (old.head === :call || old.head === :where)
        remove_linenums!(new)
        oldcall = sprint(show_unquoted, old)
        newcall = sprint(show_unquoted, new)
        # if old.head is a :where, step down one level to the :call to avoid code duplication below
        callexpr = old.head === :call ? old : old.args[1]
        if callexpr.head === :call
            fnexpr = callexpr.args[1]
            if fnexpr isa Expr && fnexpr.head === :curly
                fnexpr = fnexpr.args[1]
            end
            if export_old
                if fnexpr isa Symbol
                    maybe_export = Expr(:export, esc(fnexpr))
                else
                    cannot_export_nonsymbol()
                end
            else
                maybe_export = nothing
            end
        else
            error("invalid usage of @deprecate")
        end
        Expr(:toplevel,
            maybe_export,
            :($(esc(old)) = begin
                  $meta
                  depwarn($"`$oldcall` is deprecated, use `$newcall` instead.", Core.Typeof($(esc(fnexpr))).name.mt.name)
                  $(esc(new))
              end))
    else
        if export_old && !(old isa Symbol)
            cannot_export_nonsymbol()
        end
        Expr(:toplevel,
            export_old ? Expr(:export, esc(old)) : nothing,
            :(function $(esc(old))(args...; kwargs...)
                  $meta
                  depwarn($"`$old` is deprecated, use `$new` instead.", Core.Typeof($(esc(old))).name.mt.name)
                  $(esc(new))(args...; kwargs...)
              end))
    end
end

"""
    Base.depwarn(msg::String, funcsym::Symbol; force=false)

Print `msg` as a deprecation warning. The symbol `funcsym` should be the name
of the calling function, which is used to ensure that the deprecation warning is
only printed the first time for each call place. Set `force=true` to force the
warning to always be shown, even if Julia was started with `--depwarn=no` (the
default).

See also [`@deprecate`](@ref).

# Examples
```julia
function deprecated_func()
    Base.depwarn("Don't use `deprecated_func()`!", :deprecated_func)

    1 + 1
end
```
"""
@nospecializeinfer function depwarn(msg, funcsym; force::Bool=false)
    @nospecialize
    # N.B. With this use of `@invokelatest`, we're preventing the addition of backedges from
    # callees, such as `convert`, to this user-facing method. This approach is designed to
    # enhance the resilience of packages that utilize `depwarn` against invalidation.
    return @invokelatest _depwarn(msg, funcsym, force)
end
@nospecializeinfer function _depwarn(msg, funcsym, force::Bool)
    @nospecialize
    opts = JLOptions()
    if opts.depwarn == 2
        throw(ErrorException(msg))
    end
    deplevel = force || opts.depwarn == 1 ? CoreLogging.Warn : CoreLogging.BelowMinLevel
    @logmsg(
        deplevel,
        msg,
        _module=begin
            bt = backtrace()
            frame, caller = firstcaller(bt, funcsym)
            linfo = caller.linfo
            if linfo isa Core.MethodInstance
                def = linfo.def
                def isa Module ? def : def.module
            else
                Core    # TODO: Is it reasonable to attribute callers without linfo to Core?
            end
        end,
        _file=String(caller.file),
        _line=caller.line,
        _id=(frame,funcsym),
        _group=:depwarn,
        caller=caller,
        maxlog=funcsym === nothing ? nothing : 1
    )
    nothing
end

firstcaller(bt::Vector, ::Nothing) = Ptr{Cvoid}(0), StackTraces.UNKNOWN
firstcaller(bt::Vector, funcsym::Symbol) = firstcaller(bt, (funcsym,))
function firstcaller(bt::Vector, funcsyms)
    # Identify the calling line
    found = false
    for ip in bt
        lkups = StackTraces.lookup(ip)
        for lkup in lkups
            if lkup == StackTraces.UNKNOWN || lkup.from_c
                continue
            end
            if found
                return ip, lkup
            end
            found = lkup.func in funcsyms
            # look for constructor type name
            if !found
                li = lkup.linfo
                if li isa Core.MethodInstance
                    def = li.def
                    found = def isa Method && def.name in funcsyms
                end
            end
        end
    end
    return C_NULL, StackTraces.UNKNOWN
end

deprecate(m::Module, s::Symbol, flag=1) = ccall(:jl_deprecate_binding, Cvoid, (Any, Any, Cint), m, s, flag)

macro deprecate_binding(old, new, export_old=true, dep_message=:nothing, constant=true)
    dep_message === :nothing && (dep_message = ", use $new instead.")
    return Expr(:toplevel,
         export_old ? Expr(:export, esc(old)) : nothing,
         Expr(:const, Expr(:(=), esc(Symbol(string("_dep_message_",old))), esc(dep_message))),
         constant ? Expr(:const, Expr(:(=), esc(old), esc(new))) : Expr(:(=), esc(old), esc(new)),
         Expr(:call, :deprecate, __module__, Expr(:quote, old)))
end

macro deprecate_stdlib(old, mod, export_old=true, newname=old)
    rename = old === newname ? "" : " as `$newname`"
    dep_message = """: it has been moved to the standard library package `$mod`$rename.
                        Add `using $mod` to your imports."""
    new = GlobalRef(Base.root_module(Base, mod), newname)
    return Expr(:toplevel,
         export_old ? Expr(:export, esc(old)) : nothing,
         Expr(:const, Expr(:(=), esc(Symbol(string("_dep_message_",old))), esc(dep_message))),
         Expr(:const, Expr(:(=), esc(old), esc(new))),
         Expr(:call, :deprecate, __module__, Expr(:quote, old)))
end

macro deprecate_moved(old, new, export_old=true)
    eold = esc(old)
    emsg = string(old, " has been moved to the package ", new, ".jl.\n",
        "Run `Pkg.add(\"", new, "\")` to install it, restart Julia,\n",
        "and then run `using ", new, "` to load it.")
    return Expr(:toplevel,
        :($eold(args...; kwargs...) = error($emsg)),
        export_old ? Expr(:export, eold) : nothing,
        Expr(:call, :deprecate, __module__, Expr(:quote, old), 2))
end

# BEGIN 1.0 deprecations

@deprecate one(i::CartesianIndex)                    oneunit(i)
@deprecate one(I::Type{CartesianIndex{N}}) where {N} oneunit(I)

@deprecate BigFloat(x, prec::Int)                               BigFloat(x; precision=prec)
@deprecate BigFloat(x, prec::Int, rounding::RoundingMode)       BigFloat(x, rounding; precision=prec)
@deprecate BigFloat(x::Real, prec::Int)                         BigFloat(x; precision=prec)
@deprecate BigFloat(x::Real, prec::Int, rounding::RoundingMode) BigFloat(x, rounding; precision=prec)

# END 1.0 deprecations

# BEGIN 1.5 deprecations

"""
    isimmutable(v) -> Bool
!!! warning
    Consider using `!ismutable(v)` instead, as `isimmutable(v)` will be replaced by `!ismutable(v)` in a future release. (Since Julia 1.5)
Return `true` iff value `v` is immutable.  See [Mutable Composite Types](@ref)
for a discussion of immutability. Note that this function works on values, so if you give it
a type, it will tell you that a value of `DataType` is mutable.

# Examples
```jldoctest
julia> isimmutable(1)
true

julia> isimmutable([1,2])
false
```
"""
isimmutable(@nospecialize(x)) = !ismutable(x)
export isimmutable
# Note isimmutable is not @deprecated out of performance concerns

macro get!(h, key0, default)
    f, l = __source__.file, __source__.line
    @warn "`@get!(dict, key, default)` at $f:$l is deprecated, use `get!(()->default, dict, key)` instead."
    return quote
        get!(()->$(esc(default)), $(esc(h)), $(esc(key0)))
    end
end

pointer(V::SubArray{<:Any,<:Any,<:Array,<:Tuple{Vararg{RangeIndex}}}, is::Tuple) = pointer(V, CartesianIndex(is))

# END 1.5 deprecations

# BEGIN 1.6 deprecations

# These changed from SimpleVector to `MethodMatch`. These definitions emulate
# being a SimpleVector to ease transition for packages that make explicit
# use of (internal) APIs that return raw method matches.
iterate(match::Core.MethodMatch, field::Int=1) =
    field > nfields(match) ? nothing : (getfield(match, field), field+1)
getindex(match::Core.MethodMatch, field::Int) =
    getfield(match, field)


# these were internal functions, but some packages seem to be relying on them
tuple_type_head(T::Type) = fieldtype(T, 1)
tuple_type_cons(::Type, ::Type{Union{}}) = Union{}
@assume_effects :foldable tuple_type_cons(::Type{S}, ::Type{T}) where T<:Tuple where S =
    Tuple{S, T.parameters...}
@assume_effects :foldable parameter_upper_bound(t::UnionAll, idx) =
    rewrap_unionall((unwrap_unionall(t)::DataType).parameters[idx], t)

# these were internal functions, but some packages seem to be relying on them
@deprecate cat_shape(dims, shape::Tuple{}, shapes::Tuple...) cat_shape(dims, shapes) false
cat_shape(dims, shape::Tuple{}) = () # make sure `cat_shape(dims, ())` do not recursively calls itself

@deprecate unsafe_indices(A) axes(A) false
@deprecate unsafe_length(r) length(r) false

# these were internal type aliases, but some packages seem to be relying on them
const Any16{N} = Tuple{Any,Any,Any,Any,Any,Any,Any,Any,
                        Any,Any,Any,Any,Any,Any,Any,Any,Vararg{Any,N}}
const All16{T,N} = Tuple{T,T,T,T,T,T,T,T,
                         T,T,T,T,T,T,T,T,Vararg{T,N}}

# END 1.6 deprecations

# BEGIN 1.7 deprecations

# the plan is to eventually overload getproperty to access entries of the dict
@noinline function getproperty(x::Pairs, s::Symbol)
    depwarn("use values(kwargs) and keys(kwargs) instead of kwargs.data and kwargs.itr", :getproperty, force=true)
    return getfield(x, s)
end

# This function was marked as experimental and not exported.
@deprecate catch_stack(task=current_task(); include_bt=true) current_exceptions(task; backtrace=include_bt) false

# END 1.7 deprecations

# BEGIN 1.8 deprecations

const var"@_inline_meta" = var"@inline"
const var"@_noinline_meta" = var"@noinline"
@deprecate getindex(t::Tuple, i::Real) t[convert(Int, i)]

# END 1.8 deprecations

# BEGIN 1.9 deprecations

# We'd generally like to avoid direct external access to internal fields
# Core.Compiler.is_inlineable and Core.Compiler.set_inlineable! move towards this direction,
# but we need to keep these around for compat
function getproperty(ci::CodeInfo, s::Symbol)
    s === :inlineable && return Core.Compiler.is_inlineable(ci)
    return getfield(ci, s)
end

function setproperty!(ci::CodeInfo, s::Symbol, v)
    s === :inlineable && return Core.Compiler.set_inlineable!(ci, v)
    return setfield!(ci, s, convert(fieldtype(CodeInfo, s), v))
end

@eval Threads nthreads() = threadpoolsize()

@eval Threads begin
    """
        resize_nthreads!(A, copyvalue=A[1])

    Resize the array `A` to length [`nthreads()`](@ref).   Any new
    elements that are allocated are initialized to `deepcopy(copyvalue)`,
    where `copyvalue` defaults to `A[1]`.

    This is typically used to allocate per-thread variables, and
    should be called in `__init__` if `A` is a global constant.

    !!! warning

        This function is deprecated, since as of Julia v1.9 the number of
        threads can change at run time. Instead, per-thread state should be
        created as needed based on the thread id of the caller.
    """
    function resize_nthreads!(A::AbstractVector, copyvalue=A[1])
        nthr = nthreads()
        nold = length(A)
        resize!(A, nthr)
        for i = nold+1:nthr
            A[i] = deepcopy(copyvalue)
        end
        return A
    end
end

# END 1.9 deprecations

# BEGIN 1.10 deprecations

"""
    @pure ex

`@pure` gives the compiler a hint for the definition of a pure function,
helping for type inference.

!!! warning
    This macro is intended for internal compiler use and may be subject to changes.

!!! warning
    In Julia 1.8 and higher, it is favorable to use [`@assume_effects`](@ref) instead of `@pure`.
    This is because `@assume_effects` allows a finer grained control over Julia's purity
    modeling and the effect system enables a wider range of optimizations.
"""
macro pure(ex)
    return esc(:(Base.@assume_effects :foldable $ex))
end

# END 1.10 deprecations

# BEGIN 1.11 deprecations

# these were never a part of the public API and so they can be removed without deprecation
# in a minor release but we're being nice and trying to avoid transient breakage.
@deprecate permute!!(a, p::AbstractVector{<:Integer}) permute!(a, p) false
@deprecate invpermute!!(a, p::AbstractVector{<:Integer}) invpermute!(a, p) false

# END 1.11 deprecations
