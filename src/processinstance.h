/**
 * @file processinstance.h - class for controlling real instances of Clearwater
 * processes.
 *
 * Project Clearwater - IMS in the cloud.
 * Copyright (C) 2015  Metaswitch Networks Ltd
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version, along with the "Special Exception" for use of
 * the program along with SSL, set forth below. This program is distributed
 * in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details. You should have received a copy of the GNU General Public
 * License along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * The author can be reached by email at clearwater@metaswitch.com or by
 * post at Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
 *
 * Special Exception
 * Metaswitch Networks Ltd  grants you permission to copy, modify,
 * propagate, and distribute a work formed by combining OpenSSL with The
 * Software, or a work derivative of such a combination, even if such
 * copying, modification, propagation, or distribution would otherwise
 * violate the terms of the GPL. You must comply with the GPL in all
 * respects for all of the code used other than OpenSSL.
 * "OpenSSL" means OpenSSL toolkit software distributed by the OpenSSL
 * Project and licensed under the OpenSSL Licenses, or a work based on such
 * software and licensed under the OpenSSL Licenses.
 * "OpenSSL Licenses" means the OpenSSL License and Original SSLeay License
 * under which the OpenSSL Project distributes the OpenSSL toolkit software,
 * as those licenses appear in the file LICENSE-OPENSSL.
 */

#include <string>
#include <map>
#include <vector>

class ProcessInstance
{
public:
  ProcessInstance(std::string ip, int port) : _ip(ip), _port(port) {};
  ProcessInstance(int port) : ProcessInstance("127.0.0.1", port) {};
  virtual ~ProcessInstance() { kill_instance(); }

  bool start_instance();
  bool kill_instance();
  bool restart_instance();
  bool wait_for_instance();

  std::string ip() const { return _ip; }
  int port() const { return _port; }

private:
  virtual bool execute_process() = 0;

  std::string _ip;
  int _port;
  int _pid;
};

class MemcachedInstance : public ProcessInstance
{
public:
  MemcachedInstance(int port) : ProcessInstance(port) {};
  virtual bool execute_process();
};

class AstaireInstance : public ProcessInstance
{
public:
  AstaireInstance(const std::string& ip, int port) : ProcessInstance(ip, port) {};
  virtual bool execute_process();
};

class DnsmasqInstance : public ProcessInstance
{
public:
  DnsmasqInstance(std::string ip, int port, std::map<std::string, std::vector<std::string>> a_records) :
    ProcessInstance(ip, port) { write_config(a_records); };
  ~DnsmasqInstance() { std::remove(_cfgfile.c_str()); };

  bool execute_process();
private:
  void write_config(std::map<std::string, std::vector<std::string>> a_records);
  std::string _cfgfile;
};
