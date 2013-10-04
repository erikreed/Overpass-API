/** Copyright 2008, 2009, 2010, 2011, 2012 Roland Olbricht
*
* This file is part of Overpass_API.
*
* Overpass_API is free software: you can redistribute it and/or modify
* it under the terms of the GNU Affero General Public License as
* published by the Free Software Foundation, either version 3 of the
* License, or (at your option) any later version.
*
* Overpass_API is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU Affero General Public License
* along with Overpass_API.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "dispatcher_stub.h"
#include "../frontend/user_interface.h"
#include "../statements/statement_dump.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>


string de_escape(string input)
{
  string result;
  string::size_type pos = 0;
  while (pos < input.length())
  {
    if (input[pos] != '\\')
      result += input[pos];
    else
    {
      ++pos;
      if (pos >= input.length())
	break;
      if (input[pos] == 'n')
	result += '\n';
      else if (input[pos] == 't')
	result += '\t';
      else
	result += input[pos];
    }
    ++pos;
  }
  return result;
}


Dispatcher_Stub::Dispatcher_Stub
    (string db_dir_, Error_Output* error_output_, string xml_raw, bool uses_meta, int area_level,
     uint32 max_allowed_time, uint64 max_allowed_space)
    : db_dir(db_dir_), error_output(error_output_),
      dispatcher_client(0), area_dispatcher_client(0),
      transaction(0), area_transaction(0), rman(0), meta(uses_meta)
{
  if (db_dir == "")
  {
    uint32 client_token = probe_client_token();
    dispatcher_client = new Dispatcher_Client(osm_base_settings().shared_name);
    Logger logger(dispatcher_client->get_db_dir());
    try
    {
      logger.annotated_log("request_read_and_idx() start");
      dispatcher_client->request_read_and_idx(max_allowed_time, max_allowed_space, client_token);
      logger.annotated_log("request_read_and_idx() end");
    }
    catch (const File_Error& e)
    {
      ostringstream out;
      out<<e.origin<<' '<<e.filename<<' '<<e.error_number<<' '<<strerror(e.error_number);
      if (e.origin == "Dispatcher_Client::request_read_and_idx::rate_limited"
          || e.origin == "Dispatcher_Client::request_read_and_idx::timeout")
	out<<' '<<probe_client_identifier();
      logger.annotated_log(out.str());
      throw;
    }
    transaction = new Nonsynced_Transaction
        (false, false, dispatcher_client->get_db_dir(), "");
  
    transaction->data_index(osm_base_settings().NODES);
    transaction->random_index(osm_base_settings().NODES);
    transaction->data_index(osm_base_settings().NODE_TAGS_LOCAL);
    transaction->data_index(osm_base_settings().NODE_TAGS_GLOBAL);
    transaction->data_index(osm_base_settings().WAYS);
    transaction->random_index(osm_base_settings().WAYS);
    transaction->data_index(osm_base_settings().WAY_TAGS_LOCAL);
    transaction->data_index(osm_base_settings().WAY_TAGS_GLOBAL);
    transaction->data_index(osm_base_settings().RELATIONS);
    transaction->random_index(osm_base_settings().RELATIONS);
    transaction->data_index(osm_base_settings().RELATION_ROLES);
    transaction->data_index(osm_base_settings().RELATION_TAGS_LOCAL);
    transaction->data_index(osm_base_settings().RELATION_TAGS_GLOBAL);
    
    if (meta)
    {
      transaction->data_index(meta_settings().NODES_META);
      transaction->data_index(meta_settings().WAYS_META);
      transaction->data_index(meta_settings().RELATIONS_META);
      transaction->data_index(meta_settings().USER_DATA);
      transaction->data_index(meta_settings().USER_INDICES);
    }
    
    {
      ifstream version((dispatcher_client->get_db_dir() + "osm_base_version").c_str());
      getline(version, timestamp);
      timestamp = de_escape(timestamp);
    }
    try
    {
      logger.annotated_log("read_idx_finished() start");
      dispatcher_client->read_idx_finished();
      logger.annotated_log("read_idx_finished() end");
      logger.annotated_log('\n' + xml_raw);
    }
    catch (const File_Error& e)
    {
      ostringstream out;
      out<<e.origin<<' '<<e.filename<<' '<<e.error_number<<' '<<strerror(e.error_number);
      logger.annotated_log(out.str());
      throw;
    }
    
    if (area_level > 0)
    {
      area_dispatcher_client = new Dispatcher_Client(area_settings().shared_name);
      Logger logger(area_dispatcher_client->get_db_dir());
      
      if (area_level == 1)
      {
	try
	{
          logger.annotated_log("request_read_and_idx() area start");
	  area_dispatcher_client->request_read_and_idx(max_allowed_time, max_allowed_space, client_token);
          logger.annotated_log("request_read_and_idx() area end");
        }
	catch (const File_Error& e)
	{
	  ostringstream out;
	  out<<e.origin<<' '<<e.filename<<' '<<e.error_number<<' '<<strerror(e.error_number);
	  logger.annotated_log(out.str());
	  throw;
	}
	area_transaction = new Nonsynced_Transaction
            (false, false, area_dispatcher_client->get_db_dir(), "");
	{
	  ifstream version((area_dispatcher_client->get_db_dir() +   
	      "area_version").c_str());
	  getline(version, area_timestamp);
	  area_timestamp = de_escape(area_timestamp);
	}
      }
      else if (area_level == 2)
      {
	try
	{
	  logger.annotated_log("write_start() area start");
	  area_dispatcher_client->write_start();
	  logger.annotated_log("write_start() area end");
	}
	catch (const File_Error& e)
	{
	  ostringstream out;
	  out<<e.origin<<' '<<e.filename<<' '<<e.error_number<<' '<<strerror(e.error_number);
	  logger.annotated_log(out.str());
	  throw;
	}
	area_transaction = new Nonsynced_Transaction
	    (true, true, area_dispatcher_client->get_db_dir(), "");
	{
	  ofstream area_version((area_dispatcher_client->get_db_dir()
	      + "area_version.shadow").c_str());
	  area_version<<timestamp<<'\n';
	  area_timestamp = de_escape(timestamp);
	}
      }
      
      area_transaction->data_index(area_settings().AREAS);
      area_transaction->data_index(area_settings().AREA_BLOCKS);
      area_transaction->data_index(area_settings().AREA_TAGS_LOCAL);
      area_transaction->data_index(area_settings().AREA_TAGS_GLOBAL);

      if (area_level == 1)
      {
	try
	{
          logger.annotated_log("read_idx_finished() area start");
          area_dispatcher_client->read_idx_finished();
          logger.annotated_log("read_idx_finished() area end");
	}
	catch (const File_Error& e)
	{
	  ostringstream out;
	  out<<e.origin<<' '<<e.filename<<' '<<e.error_number<<' '<<strerror(e.error_number);
	  logger.annotated_log(out.str());
	  throw;
	}
      }

      rman = new Resource_Manager(*transaction, area_level == 2 ? error_output : 0,
	  *area_transaction, this, area_level == 2 ? new Area_Updater(*area_transaction) : 0);
    }
    else
      rman = new Resource_Manager(*transaction, this, error_output);
  }
  else
  {
    transaction = new Nonsynced_Transaction(false, false, db_dir, "");
    if (area_level > 0)
    {
      area_transaction = new Nonsynced_Transaction(area_level == 2, false, db_dir, "");
      rman = new Resource_Manager(*transaction, area_level == 2 ? error_output : 0,
	  *area_transaction, this, area_level == 2 ? new Area_Updater(*area_transaction) : 0);
    }
    else
      rman = new Resource_Manager(*transaction, this, error_output);
    
    {
      ifstream version((db_dir + "osm_base_version").c_str());
      getline(version, timestamp);
      timestamp = de_escape(timestamp);
    }
    if (area_level == 1)
    {
      ifstream version((db_dir + "area_version").c_str());
      getline(version, area_timestamp);
      area_timestamp = de_escape(timestamp);
    }
    else if (area_level == 2)
    {
      ofstream area_version((db_dir + "area_version").c_str());
      area_version<<timestamp<<'\n';
      area_timestamp = de_escape(timestamp);
    }
  }
}


void Dispatcher_Stub::ping() const
{
  if (dispatcher_client)
    dispatcher_client->ping();
  if (area_dispatcher_client)
    area_dispatcher_client->ping();
}


Dispatcher_Stub::~Dispatcher_Stub()
{
  bool areas_written = (rman->area_updater() != 0);
  delete rman;
  if (transaction)
    delete transaction;
  if (area_transaction)
    delete area_transaction;
  if (dispatcher_client)
  {
    Logger logger(dispatcher_client->get_db_dir());
    try
    {
      logger.annotated_log("read_finished() start");
      dispatcher_client->read_finished();
      logger.annotated_log("read_finished() end");
    }
    catch (const File_Error& e)
    {
      ostringstream out;
      out<<e.origin<<' '<<e.filename<<' '<<e.error_number<<' '<<strerror(e.error_number);
      logger.annotated_log(out.str());
    }
    delete dispatcher_client;
  }
  if (area_dispatcher_client)
  {
    if (areas_written)
    {
      Logger logger(area_dispatcher_client->get_db_dir());
      try
      {
        logger.annotated_log("write_commit() area start");
        area_dispatcher_client->write_commit();
        rename((area_dispatcher_client->get_db_dir() + "area_version.shadow").c_str(),
	       (area_dispatcher_client->get_db_dir() + "area_version").c_str());
        logger.annotated_log("write_commit() area end");
      }
      catch (const File_Error& e)
      {
        ostringstream out;
        out<<e.origin<<' '<<e.filename<<' '<<e.error_number<<' '<<strerror(e.error_number);
        logger.annotated_log(out.str());
      }
    }
    else
    {
      Logger logger(area_dispatcher_client->get_db_dir());
      try
      {
        logger.annotated_log("read_finished() area start");
        area_dispatcher_client->read_finished();
        logger.annotated_log("read_finished() area end");
      }
      catch (const File_Error& e)
      {
        ostringstream out;
        out<<e.origin<<' '<<e.filename<<' '<<e.error_number<<' '<<strerror(e.error_number);
        logger.annotated_log(out.str());
      }
    }
    delete area_dispatcher_client;
  }
}
