

#include "Command.hpp"
#include "Common.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <mpi.h>
#include <iterator>
#include <algorithm>

const string python = "python";
const string pythonUseCommand = "-c";
const string pythonCommand = 
"import sys\n"
"import subprocess\n"
"import os\n"
"commands = sys.argv[2:]\n"
"FNULL = open(os.devnull, 'w')\n"
"subprocess.check_call(commands, stdout=FNULL, stderr=FNULL)\n"
"f = open(sys.argv[1], \"w\")\n";


Command::Command(const string &id, const string &command):
  _id(id),
  _command(command),
  _ranksNumber(1),
  _startRank(0)
{

}

string Command::toString() const 
{
  string res;
  res = getId() + " " + getCommand();
  res += " {ranks: " + to_string(getRanksNumber()) + "}";
  return res;
}
  
void Command::execute(const string &outputDir, int startingRank, int ranksNumber)
{
  if (ranksNumber == 0) {
    throw MultiRaxmlException("Error in Command::execute: invalid number of ranks ", to_string(ranksNumber));
  }
  _beginTime = Common::getTime();
  _startRank = startingRank;
  _ranksNumber = ranksNumber;
  istringstream iss(getCommand());
  vector<string> splitCommand(istream_iterator<string>{iss},
                                       istream_iterator<string>());
  char **argv = new char*[splitCommand.size() + 3];
  string infoFile = outputDir + "/" + getId(); // todobenoit not portable
  argv[0] = (char*)pythonUseCommand.c_str();
  argv[1] = (char*)pythonCommand.c_str();
  argv[2] = (char*)infoFile.c_str();
  unsigned int offset = 3;
  for(unsigned int i = 0; i < splitCommand.size(); ++i)
    argv[i + offset] = (char*)splitCommand[i].c_str();
  argv[splitCommand.size() + offset] = 0;

  Timer t;
  MPI_Comm intercomm;
  MPI_Comm_spawn((char*)python.c_str(), argv, getRanksNumber(),  
          MPI_INFO_NULL, 0, MPI_COMM_SELF, &intercomm,  
          MPI_ERRCODES_IGNORE);
  cout << "submit time " << t.getElapsedMs() << endl;
  delete[] argv;
}
  
void Command::onFinished()
{
  _endTime = Common::getTime();
}

// Read a line in a commands file and skip comments
// discards empty lines
bool readNextLine(ifstream &is, string &os)
{
  while (getline(is, os)) {
    auto end = os.find("#");
    if (string::npos != end)
      os = os.substr(0, end);
    if (os.size()) 
      return true;
  }
  return false;
}

CommandsContainer::CommandsContainer(const string &commandsFilename)
{
  ifstream reader(commandsFilename);
  if (!reader)
    throw MultiRaxmlException("Cannot open commands file ", commandsFilename);
  
  string line;
  while (readNextLine(reader, line)) {
    auto firstSpace = line.find(" ");
    if (string::npos == firstSpace) 
      throw MultiRaxmlException("Invalid syntax in commands file ", commandsFilename);
    CommandPtr command(new Command(line.substr(0, firstSpace), line.substr(firstSpace + 1)));
    addCommand(command);
  }
}

void CommandsContainer::addCommand(CommandPtr command)
{
  _commands.push_back(command); 
  _dicoCommands[command->getId()] = command;
}

CommandPtr CommandsContainer::getCommand(string id) const
{
  auto res =  _dicoCommands.find(id);
  if (res == _dicoCommands.end())
    return CommandPtr(0);
  else 
    return res->second;
}

RanksAllocator::RanksAllocator(int availableRanks):
  _ranksInUse(0)
{
  _slots.push(std::pair<int,int>(1, availableRanks));
}


bool RanksAllocator::ranksAvailable()
{
  return !_slots.empty();
}
 
bool RanksAllocator::allRanksAvailable()
{
  return _ranksInUse == 0;
}
 
void RanksAllocator::allocateRanks(int requestedRanks, 
      int &startingRank,
      int &threadsNumber)
{
  std::pair<int, int> result = _slots.top();
  _slots.pop();
  startingRank = result.first;
  threadsNumber = requestedRanks; 
  // border case around the first rank
  if (result.first == 1 && requestedRanks != 1) {
    threadsNumber -= 1; 
  }
  if (result.second > threadsNumber) {
    result.first += threadsNumber;
    result.second -= threadsNumber;
    _slots.push(result);
  }
  _ranksInUse += threadsNumber;
}
  
void RanksAllocator::freeRanks(int startingRank, int ranksNumber)
{
  _ranksInUse -= ranksNumber;
  _slots.push(std::pair<int,int>(startingRank, ranksNumber));
}

CommandsRunner::CommandsRunner(const CommandsContainer &commandsContainer,
      unsigned int availableRanks,
      const string &outputDir):
  _commandsContainer(commandsContainer),
  _availableRanks(availableRanks),
  _outputDir(outputDir),
  _allocator(availableRanks),
  _commandsVector(commandsContainer.getCommands()),
  _commandIterator(_commandsVector.begin())
{
  
}

void CommandsRunner::run() 
{
  Timer timer;
  while (!_allocator.allRanksAvailable() || !isCommandsEmpty()) {
    if (!isCommandsEmpty()) {
      auto  nextCommand = getPendingCommand();
      if (_allocator.ranksAvailable()) {
        executePendingCommand();
        continue;
      }
    }
    checkCommandsFinished();
    //Common::sleep(10);
  }
}
  
void CommandsRunner::executePendingCommand()
{
  auto command = getPendingCommand();
  int startingRank;
  int allocatedRanks;
  _allocator.allocateRanks(command->getRanksNumber(), startingRank, allocatedRanks);
  cout << "Executing command " << command->toString() << " on " << allocatedRanks << " ranks"  << endl;
  command->execute(getOutputDir(), startingRank, allocatedRanks);
  _commandIterator++;
}

void CommandsRunner::checkCommandsFinished()
{
  vector<string> files;
  Common::readDirectory(_outputDir, files);
  for (auto file: files) {
    CommandPtr command = _commandsContainer.getCommand(file);
    if (command) {
      onCommandFinished(command);
    }
  }
}

void CommandsRunner::onCommandFinished(CommandPtr command)
{
  command->onFinished();
  string fullpath = _outputDir + "/" + command->getId(); // todobenoit not portable
  Common::removefile(fullpath);
  _allocator.freeRanks(command->getStartRank(), command->getRanksNumber());
  cout << "Command " << command->getId() << " finished after ";
  cout << command->getElapsedMs() << "ms" << endl;
}

CommandsStatistics::CommandsStatistics(const CommandsContainer &commands,
    Time begin,
    Time end,
    int availableRanks):
  _commands(commands),
  _begin(begin),
  _end(end),
  _availableRanks(availableRanks)
{

}

void CommandsStatistics::printGeneralStatistics()
{
  int totalElapsedTime = Common::getElapsedMs(_begin, _end);
  int cumulatedTime = 0;
  int longestTime = 0;
  for (auto command: _commands.getCommands()) {
    cumulatedTime += command->getElapsedMs() * command->getRanksNumber();
    longestTime = max(longestTime, command->getElapsedMs());
  }
  double ratio = double(cumulatedTime) / double(_availableRanks * totalElapsedTime);
  
  cout << "Finished running commands. Total elasped time: ";
  cout << totalElapsedTime << "ms" << endl;
  cout << "The longest command took " << longestTime << "ms" << endl;
  cout << "Load balance ratio: " << ratio << endl;
}

string get_random_hex()
{
  static const char *buff = "0123456789abcdef";
  char res[8];
  res[0] = '#';
  res[7] = 0;
  for (int i = 0; i < 6; ++i) {
    res[i+1] = buff[rand() % 16];
  }
  return string(res);
}

void CommandsStatistics::exportSVG(const string &svgfile) 
{
  cout << "Saving svg output in " << svgfile << endl;
  ofstream os(svgfile, std::ofstream::out);
  if (!os) {
    cerr << "Warning: cannot open  " << svgfile << ". Skipping svg export." << endl;
    return; 
  }
  int totalWidth = _availableRanks + 1;
  int totalHeight = Common::getElapsedMs(_begin, _end);
  double ratioWidth = 500.0 / double(totalWidth);
  double ratioHeight = 500.0 / double(totalHeight);
   
  os << "<svg  xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\">" << endl;
  os << "  <svg x=\"0\" y=\"0.0\" width=\"" << ratioWidth << "\" height=\"500.0\" >" << endl;
  os << "    <rect x=\"0%\" y=\"0%\" height=\"100%\" width=\"100%\" style=\"fill: #ffffff\"/>" << endl;
  os << "  </svg>" << endl;

  for (auto command: _commands.getCommands()) {
    os << "  <svg x=\"" << ratioWidth * command->getStartRank()
       << "\" y=\"" << ratioHeight * Common::getElapsedMs(_begin, command->getStartTime()) << "\" "
       << "width=\"" << ratioWidth * command->getRanksNumber() << "\" " 
       << "height=\""  << ratioHeight * command->getElapsedMs() << "\" >" << endl;
    string color = get_random_hex(); 
    os << "    <rect x=\"0%\" y=\"0%\" height=\"100%\" width=\"100%\" style=\"fill: "
       << color  <<  "\"/>" << endl;
    os << "  </svg>" << endl;
  }
  os << "</svg>" << endl;
}

