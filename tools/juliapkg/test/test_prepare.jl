# test_prepare.jl

@testset "Test DBInterface.prepare" begin
    con = DBInterface.connect(DuckDB.DB)

    DBInterface.execute(con, "CREATE TABLE test_table(i INTEGER, j DOUBLE)")
    stmt = DBInterface.prepare(con, "INSERT INTO test_table VALUES(?, ?)")

    DBInterface.execute(stmt, [1, 3.5])
    DBInterface.execute(stmt, [missing, nothing])
    DBInterface.execute(stmt, [2, 0.5])

    results = DBInterface.execute(con, "SELECT * FROM test_table")
    df = DataFrame(results)

    @test isequal(df.i, [1, missing, 2])
    @test isequal(df.j, [3.5, missing, 0.5])

    # execute many
    DBInterface.executemany(stmt, (col1 = [1, 2, 3, 4, 5], col2 = [1, 2, 4, 8, -0.5]))

    results = DBInterface.execute(con, "SELECT * FROM test_table")
    df = DataFrame(results)

    @test isequal(df.i, [1, missing, 2, 1, 2, 3, 4, 5])
    @test isequal(df.j, [3.5, missing, 0.5, 1, 2, 4, 8, -0.5])

    # can bind vectors to parameters
    stmt = DBInterface.prepare(con, "FROM test_table WHERE i IN ?;")
    results = DBInterface.execute(stmt, ([1, 2],))
    df = DataFrame(results)

    @test all(df.i .∈ Ref([1, 2]))

    # verify that double-closing does not cause any problems
    DBInterface.close!(stmt)
    DBInterface.close!(stmt)
    DBInterface.close!(con)
    DBInterface.close!(con)
end

@testset "Test DBInterface.prepare with various types" begin
    con = DBInterface.connect(DuckDB.DB)

    type_names = [
        "BOOLEAN",
        "TINYINT",
        "SMALLINT",
        "INTEGER",
        "BIGINT",
        "UTINYINT",
        "USMALLINT",
        "UINTEGER",
        "UBIGINT",
        "FLOAT",
        "DOUBLE",
        "DATE",
        "TIME",
        "TIMESTAMP",
        "VARCHAR",
        "INTEGER",
        "BLOB"
    ]
    type_values = [
        Bool(true),
        Int8(3),
        Int16(4),
        Int32(8),
        Int64(20),
        UInt8(42),
        UInt16(300),
        UInt32(420421),
        UInt64(43294832),
        Float32(0.5),
        Float64(0.25),
        Date(1992, 9, 20),
        Time(23, 10, 33),
        DateTime(1992, 9, 20, 23, 10, 33),
        String("hello world"),
        missing,
        rand(UInt8, 100)
    ]
    for i in 1:size(type_values, 1)
        stmt = DBInterface.prepare(con, string("SELECT ?::", type_names[i], " a"))
        result = DataFrame(DBInterface.execute(stmt, [type_values[i]]))
        @test isequal(result.a, [type_values[i]])
    end
end

@testset "DBInterface.prepare: named parameters not supported yet" begin
    con = DBInterface.connect(DuckDB.DB)

    DBInterface.execute(con, "CREATE TABLE test_table(i INTEGER, j DOUBLE)")
    @test_throws DuckDB.QueryException DBInterface.prepare(con, "INSERT INTO test_table VALUES(:col1, :col2)")

    DBInterface.close!(con)
end

@testset "prepare: Named parameters" begin
    con = DBInterface.connect(DuckDB.DB)
    DBInterface.execute(con, "CREATE TABLE test_table(i INTEGER, j DOUBLE)")

    # Check named syntax with Kwargs and Dict
    stmt = DBInterface.prepare(con, raw"INSERT INTO test_table VALUES($col1, $col2)")
    DBInterface.execute(stmt, Dict(["col1" => 1, "col2" => 3.5]))
    DBInterface.execute(stmt; col1 = 2, col2 = 4.5)
    results = DBInterface.execute(con, "SELECT * FROM test_table") |> DataFrame
    @test isequal(results.i, [1, 2])
    @test isequal(results.j, [3.5, 4.5])


    # Check positional syntax
    DBInterface.execute(con, "TRUNCATE TABLE test_table")
    stmt = DBInterface.prepare(con, raw"INSERT INTO test_table VALUES($2, $1)")
    DBInterface.execute(stmt, (3.5, 1))
    DBInterface.execute(stmt, (4.5, 2))
    results = DBInterface.execute(con, "SELECT * FROM test_table") |> DataFrame
    @test isequal(results.i, [1, 2])
    @test isequal(results.j, [3.5, 4.5])

    DBInterface.close!(con)
end

@testset "DBInterface.prepare: execute many" begin
    con = DBInterface.connect(DuckDB.DB)

    DBInterface.execute(con, "CREATE TABLE test_table(i INTEGER, j DOUBLE)")
    @test_throws DuckDB.QueryException DBInterface.prepare(con, "INSERT INTO test_table VALUES(:col1, :col2)")
    stmt = DBInterface.prepare(con, raw"INSERT INTO test_table VALUES($col1, $col2)")
    col1 = [1, 2, 3, 4, 5]
    col2 = [1, 2, 4, 8, -0.5]
    DBInterface.executemany(stmt, (col1 = col1, col2 = col2))
    results = DBInterface.execute(con, "SELECT * FROM test_table") |> DataFrame

    @test isequal(results.i, col1)
    @test isequal(results.j, col2)

    DBInterface.close!(con)
end


@testset "DBInterface.prepare: ambiguous parameters" begin
    con = DBInterface.connect(DuckDB.DB)

    stmt = DBInterface.prepare(con, "SELECT ? AS a")
    result = DataFrame(DBInterface.execute(stmt, [42]))
    @test isequal(result.a, [42])

    result = DataFrame(DBInterface.execute(stmt, ["hello world"]))
    @test isequal(result.a, ["hello world"])

    result = DataFrame(DBInterface.execute(stmt, [DateTime(1992, 9, 20, 23, 10, 33)]))
    @test isequal(result.a, [DateTime(1992, 9, 20, 23, 10, 33)])
end
