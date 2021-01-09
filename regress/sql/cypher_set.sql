/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

LOAD 'age';
SET search_path TO ag_catalog;

SELECT create_graph('cypher_set');

SELECT * FROM cypher('cypher_set', $$CREATE (:v)$$) AS (a agtype);
SELECT * FROM cypher('cypher_set', $$CREATE (:v {i: 0, j: 5, a: 0})$$) AS (a agtype);
SELECT * FROM cypher('cypher_set', $$CREATE (:v {i: 1})$$) AS (a agtype);

--Simple SET test case
SELECT * FROM cypher('cypher_set', $$MATCH (n) SET n.i = 3$$) AS (a agtype);

SELECT * FROM cypher('cypher_set', $$MATCH (n) WHERE n.j = 5 SET n.i = NULL RETURN n$$) AS (a agtype);
SELECT * FROM cypher('cypher_set', $$MATCH (n) RETURN n$$) AS (a agtype);

SELECT * FROM cypher('cypher_set', $$MATCH (n) SET n.i = NULL RETURN n$$) AS (a agtype);
SELECT * FROM cypher('cypher_set', $$MATCH (n) RETURN n$$) AS (a agtype);

SELECT * FROM cypher('cypher_set', $$MATCH (n) SET n.i = 3 RETURN n$$) AS (a agtype);
SELECT * FROM cypher('cypher_set', $$MATCH (n) RETURN n$$) AS (a agtype);

--Handle Inheritance
SELECT * FROM cypher('cypher_set', $$CREATE ()$$) AS (a agtype);
SELECT * FROM cypher('cypher_set', $$MATCH (n) SET n.i = 3 RETURN n$$) AS (a agtype);
SELECT * FROM cypher('cypher_set', $$MATCH (n) RETURN n$$) AS (a agtype);

--Validate Paths are updated
SELECT * FROM cypher('cypher_set', $$MATCH (n) CREATE (n)-[:e {j:20}]->(:other_v {k:10}) RETURN n$$) AS (a agtype);
SELECT * FROM cypher('cypher_set', $$MATCH p=(n)-[]->() SET n.i = 50 RETURN p$$) AS (a agtype);

--Edges
SELECT * FROM cypher('cypher_set', $$MATCH ()-[n]-(:other_v) SET n.i = 3 RETURN n$$) AS (a agtype);
SELECT * FROM cypher('cypher_set', $$MATCH ()-[n]->(:other_v) RETURN n$$) AS (a agtype);

SELECT * FROM cypher('cypher_set', $$
        MATCH (n {j: 5})
        SET n.y = 50
        SET n.z = 99
        RETURN n
$$) AS (a agtype);

SELECT * FROM cypher('cypher_set', $$
        MATCH (n {j: 5})
        RETURN n
$$) AS (a agtype);

--Create a loop and see that set can work after create
SELECT * FROM cypher('cypher_set', $$
	MATCH (n {j: 5})
	CREATE p=(n)-[e:e {j:34}]->(n)
	SET n.y = 99
	RETURN n, p
$$) AS (a agtype, b agtype);

--Create a loop and see that set can work after create
SELECT * FROM cypher('cypher_set', $$
	CREATE ()-[e:e {j:34}]->()
	SET e.y = 99
	RETURN e
$$) AS (a agtype);

SELECT * FROM cypher('cypher_set', $$
        MATCH (n)
        MATCH (n)-[e:e {j:34}]->()
        SET n.y = 1
        RETURN n
$$) AS (a agtype);

SELECT * FROM cypher('cypher_set', $$
        MATCH (n)
        MATCH ()-[e:e {j:34}]->(n)
        SET n.y = 2
        RETURN n
$$) AS (a agtype);

SELECT * FROM cypher('cypher_set', $$MATCH (n)-[]->(n) SET n.y = 99 RETURN n$$) AS (a agtype);

SELECT * FROM cypher('cypher_set', $$MATCH (n) MATCH (n)-[]->(m) SET n.t = 150 RETURN n$$) AS (a agtype);

--Errors
SELECT * FROM cypher('cypher_set', $$SET n.i = NULL$$) AS (a agtype);

SELECT * FROM cypher('cypher_set', $$MATCH (n) SET wrong_var.i = 3$$) AS (a agtype);

SELECT * FROM cypher('cypher_set', $$MATCH (n) SET n.i = 3, n.j = 5 $$) AS (a agtype);

--
-- Clean up
--

SELECT drop_graph('cypher_set', true);

--
-- End
--
