#include "../../expat/expat_justparse_interface.h"
#include "../../template_db/dispatcher.h"
#include "../frontend/console_output.h"
#include "../frontend/user_interface.h"
#include "../statements/statement.h"
#include "resource_manager.h"
#include "scripting_core.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

template< class TIndex >
struct Flat_Meta_Collector
{
  public:
    Flat_Meta_Collector(Transaction& transaction, const File_Properties* meta_file_prop = 0);
    
    void reset();
    const OSM_Element_Metadata_Skeleton* get(const TIndex& index, uint32 ref);    
    
    ~Flat_Meta_Collector()
    {
      if (meta_db)
      {
	delete db_it;
	delete meta_db;
      }
    }
  
  private:
    Block_Backend< TIndex, OSM_Element_Metadata_Skeleton >* meta_db;
    typename Block_Backend< TIndex, OSM_Element_Metadata_Skeleton >::Flat_Iterator*
      db_it;
    TIndex* current_index;
    map< OSM_Element_Metadata_Skeleton, bool > current_objects;
};

/** Implementation --------------------------------------------------------- */

template< class TIndex >
Flat_Meta_Collector< TIndex >::Flat_Meta_Collector
    (Transaction& transaction, const File_Properties* meta_file_prop)
  : meta_db(0), db_it(0), current_index(0)
{
  meta_db = new Block_Backend< TIndex, OSM_Element_Metadata_Skeleton >
      (transaction.data_index(meta_file_prop));
	  
  reset();
}
    
template< class TIndex >
void Flat_Meta_Collector< TIndex >::reset()
{
  if (!meta_db)
    return;
      
  if (db_it)
    delete db_it;
  if (current_index)
  {
    delete current_index;
    current_index = 0;
  }
  
  db_it = new typename Block_Backend< TIndex, OSM_Element_Metadata_Skeleton >
      ::Flat_Iterator(meta_db->flat_begin());
	
  if (!(*db_it == meta_db->flat_end()))
    current_index = new TIndex(db_it->index());
  while (!(*db_it == meta_db->flat_end()) && (*current_index == db_it->index()))
  {
    current_objects.insert(make_pair(db_it->object(), false));
    ++(*db_it);
  }
}

template< class TIndex >
const OSM_Element_Metadata_Skeleton* Flat_Meta_Collector< TIndex >::get
    (const TIndex& index, uint32 ref)
{
  if (!meta_db)
    return 0;
  
  if ((current_index) && (*current_index < index))
  {
    for (map< OSM_Element_Metadata_Skeleton, bool >::const_iterator
        it = current_objects.begin(); it != current_objects.end(); ++it)
    {
      if (!it->second)
        cout<<"Skipping meta data of "<<dec<<it->first.ref
            <<" at "<<hex<<current_index->val()<<'\n';
    }
    current_objects.clear();
    
    while (!(*db_it == meta_db->flat_end()) && (db_it->index() < index))
    {
      cout<<"Skipping meta data of "<<dec<<db_it->object().ref
          <<" at "<<hex<<db_it->index().val()<<'\n';
      ++(*db_it);
    }
    if (!(*db_it == meta_db->flat_end()))
      *current_index = db_it->index();
    while (!(*db_it == meta_db->flat_end()) && (*current_index == db_it->index()))
    {
      current_objects.insert(make_pair(db_it->object(), false));
      ++(*db_it);
    }
  }
  
  map< OSM_Element_Metadata_Skeleton, bool >::iterator it
      = current_objects.find(OSM_Element_Metadata_Skeleton(ref));
  it->second = true;
  if (it != current_objects.end())
    return &it->first;
  else
    return 0;
}

//-----------------------------------------------------------------------------

int main(int argc, char *argv[])
{ 
  // read command line arguments
  string db_dir;
  uint log_level = Error_Output::ASSISTING;

  const int META = 1;
  int mode = META;
  int area_level = 0;

  int argpos = 1;
  while (argpos < argc)
  {
    if (!(strncmp(argv[argpos], "--db-dir=", 9)))
    {
      db_dir = ((string)argv[argpos]).substr(9);
      if ((db_dir.size() > 0) && (db_dir[db_dir.size()-1] != '/'))
	db_dir += '/';
    }
    else if (!(strcmp(argv[argpos], "--meta")))
      mode = META;
    else if (!(strcmp(argv[argpos], "--quiet")))
      log_level = Error_Output::QUIET;
    else if (!(strcmp(argv[argpos], "--concise")))
      log_level = Error_Output::CONCISE;
    else if (!(strcmp(argv[argpos], "--progress")))
      log_level = Error_Output::PROGRESS;
    else if (!(strcmp(argv[argpos], "--verbose")))
      log_level = Error_Output::VERBOSE;
    ++argpos;
  }
  
  Error_Output* error_output(new Console_Output(log_level));
  Statement::set_error_output(error_output);
  
  // connect to dispatcher and get database dir
  try
  {
    // open read transaction and log this.
    Dispatcher_Stub dispatcher(db_dir, error_output, "-- consistency check --\n", area_level);
    Resource_Manager& rman = dispatcher.resource_manager();
    
    // set limits - short circuited until forecast gets effective
    dispatcher.set_limits();

    // perform check
    {
      uint32 count = 0;
      Flat_Meta_Collector< Uint32_Index > meta_collector
          (*rman.get_transaction(), meta_settings().NODES_META);
      Block_Backend< Uint32_Index, Node_Skeleton > db
          (rman.get_transaction()->data_index(osm_base_settings().NODES));
      for (Block_Backend< Uint32_Index, Node_Skeleton >::Flat_Iterator
          it(db.flat_begin()); !(it == db.flat_end()); ++it)
      {
        if (++count >= 1000*1000)
        {
	  dispatcher.ping();
          count = 0;
          cout<<"Processed 1000000 nodes.\n";
        }
        if (!meta_collector.get(it.index(), it.object().id))
	  cout<<"Missing meta data of "<<dec<<it.object().id
	      <<" at "<<hex<<it.index().val()<<'\n';
      }
    }

    {
      uint32 count = 0;
      Flat_Meta_Collector< Uint31_Index > meta_collector
          (*rman.get_transaction(), meta_settings().WAYS_META);
      Block_Backend< Uint31_Index, Way_Skeleton > db
          (rman.get_transaction()->data_index(osm_base_settings().WAYS));
      for (Block_Backend< Uint31_Index, Way_Skeleton >::Flat_Iterator
          it(db.flat_begin()); !(it == db.flat_end()); ++it)
      {
        if (++count >= 100*1000)
        {
	  dispatcher.ping();
	  count = 0;
          cout<<"Processed 100000 ways.\n";
        }
        if (!meta_collector.get(it.index(), it.object().id))
	  cout<<"Missing meta data of "<<dec<<it.object().id
	      <<" at "<<hex<<it.index().val()<<'\n';
      }
    }

    {
      uint32 count = 0;
      Flat_Meta_Collector< Uint31_Index > meta_collector
          (*rman.get_transaction(), meta_settings().RELATIONS_META);
      Block_Backend< Uint31_Index, Relation_Skeleton > db
          (rman.get_transaction()->data_index(osm_base_settings().RELATIONS));
      for (Block_Backend< Uint31_Index, Relation_Skeleton >::Flat_Iterator
          it(db.flat_begin()); !(it == db.flat_end()); ++it)
      {
        if (++count >= 100*1000)
        {
	  dispatcher.ping();
	  count = 0;
          cout<<"Processed 100000 relations.\n";
        }
        if (!meta_collector.get(it.index(), it.object().id))
	  cout<<"Missing meta data of "<<dec<<it.object().id
	      <<" at "<<hex<<it.index().val()<<'\n';
      }
    }

    cout<<"done\n";

    return 0;
  }
  catch(File_Error e)
  {
    ostringstream temp;
    if (e.origin != "Dispatcher_Stub::Dispatcher_Stub::1")
    {
      temp<<"open64: "<<e.error_number<<' '<<e.filename<<' '<<e.origin;
      if (error_output)
        error_output->runtime_error(temp.str());
    }
    
    return 1;
  }
  catch(Resource_Error e)
  {
    ostringstream temp;
    if (e.timed_out)
      temp<<"Query timed out in \""<<e.stmt_name<<"\" at line "<<e.line_number
          <<" after "<<e.runtime<<" seconds.";
    else
      temp<<"Query run out of memory in \""<<e.stmt_name<<"\" at line "
          <<e.line_number<<" using about "<<e.size/(1024*1024)<<" MB of RAM.";
    if (error_output)
      error_output->runtime_error(temp.str());
    
    return 2;
  }
  catch(Exit_Error e)
  {
    return 3;
  }
}