#include <vw/Plate/Datastore.h>
#include <vw/Plate/HTTPUtils.h>
#include <vw/Plate/detail/Blobstore.h>
#include <vw/Plate/detail/Dirstore.h>

namespace vw { namespace platefile {

Datastore* Datastore::open(const Url& url) {
  if (url.scheme() == "dir")
    return new detail::Dirstore(url);
  else
    return new detail::Blobstore(url);
  //vw_throw(NoImplErr() << "Unsupported datastore scheme: " << url.scheme());
}

Datastore* Datastore::open(const Url& url, const IndexHeader& d) {
  if (url.scheme() == "dir")
    return new detail::Dirstore(url, d);
  else
    return new detail::Blobstore(url, d);
  //vw_throw(NoImplErr() << "Unsupported datastore scheme: " << url.scheme());
}

Datastore::TileSearch& Datastore::get(TileSearch& buf, uint32 level, uint32 row, uint32 col, TransactionRange range, uint32 limit) {
  this->head(buf, level, row, col, range, limit);
  return this->populate(buf);
}

Datastore::TileSearch& Datastore::get(TileSearch& buf, uint32 level,   const BBox2u& region, TransactionRange range, uint32 limit) {
  this->head(buf, level, region, range, limit);
  return this->populate(buf);
}

}}
