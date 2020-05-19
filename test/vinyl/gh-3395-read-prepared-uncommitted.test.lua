-- Disk scan in vinyl is known to yield during read. So it opens
-- a window for modifications of in-memory level. Let's test
-- following case: right before data selection tuple is inserted
-- into space. It passes first stage of commit procedure, i.e. it
-- is prepared to be committed but still not yet reached WAL.
-- Meanwhile we are starting read of the same key. At this moment
-- prepared statement is already inserted to in-memory tree. So,
-- read iterator fetches this statement and proceeds to disk scan.
-- In turn, disk scan yields and in this moment WAL fails to write
-- statement on disk, so it is roll-backed. Read iterator must
-- recognize this situation and must not include roll-backed tuple
-- into result set.
--
fiber = require('fiber')

errinj = box.error.injection
s = box.schema.create_space('test', {engine = 'vinyl'})
pk = s:create_index('pk')
sk = s:create_index('sk', {parts = {{2, 'unsigned'}}, unique = false})
s:replace{3, 2}
s:replace{2, 2}
box.snapshot()
-- Tuple {1, 2} is not going to be committed, so it should not
-- appear in the result set.
--
c = fiber.channel(1)
function do_write() s:replace{1, 2} end
errinj.set("ERRINJ_WAL_DELAY", true)
writer = fiber.create(do_write)

function do_read() local ret = sk:select{2} c:put(ret) end
errinj.set("ERRINJ_VY_READ_PAGE_DELAY", true)
f = fiber.create(do_read)
errinj.set("ERRINJ_WAL_WRITE", true)
errinj.set("ERRINJ_WAL_DELAY", false)
errinj.set("ERRINJ_VY_READ_PAGE_DELAY", false)

c:get()

errinj.set("ERRINJ_WAL_WRITE", false)
s:drop()
