#pragma ident "$Id$"

//============================================================================
//
//  This file is part of GPSTk, the GPS Toolkit.
//
//  The GPSTk is free software; you can redistribute it and/or modify
//  it under the terms of the GNU Lesser General Public License as published
//  by the Free Software Foundation; either version 2.1 of the License, or
//  any later version.
//
//  The GPSTk is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with GPSTk; if not, write to the Free Software Foundation,
//  Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//  
//  Copyright 2004, The University of Texas at Austin
//
//============================================================================

//============================================================================
//
//This software developed by Applied Research Laboratories at the University of
//Texas at Austin, under contract to an agency or agencies within the U.S. 
//Department of Defense. The U.S. Government retains all rights to use,
//duplicate, distribute, disclose, or release this software. 
//
//Pursuant to DoD Directive 523024 
//
// DISTRIBUTION STATEMENT A: This software has been approved for public 
//                           release, distribution is unlimited.
//
//=============================================================================

/** @file Converts an MDP stream into FIC nav file */

#include "StringUtils.hpp"
#include "LoopedFramework.hpp"

#include "FICStream.hpp"
#include "FICHeader.hpp"
#include "FICData.hpp"

#include "MDPStream.hpp"
#include "MDPNavSubframe.hpp"
#include "MDPObsEpoch.hpp"
#include "FICData109.hpp"
#include "FICData9.hpp"
#include "BELogEntry.hpp"
#include "UniqueAlmStore.hpp"
#include "miscdefs.hpp"
#include "TimeString.hpp"
#include "GPSWeekSecond.hpp"

//#include "FormatConversionFunctions.hpp"

using namespace std;
using namespace gpstk;


class MDP2FIC : public gpstk::LoopedFramework
{
public:
   MDP2FIC(const std::string& applName)
      throw()
      : LoopedFramework(applName, "Converts an MDP stream to FIC."),
        navFileOpt('n', "nav",   "Filename to which FIC nav data will be written.", true),
        mdpFileOpt('i', "mdp-input", "Filename to read MDP data from. The filename of '-' means to use stdin.", true),
        logFileOpt('l', "log", "Filename for (optional) output log file",false)
   {
      navFileOpt.setMaxCount(1);
      mdpFileOpt.setMaxCount(1);
   }

   bool initialize(int argc, char *argv[]) throw()
   {
      if (!LoopedFramework::initialize(argc,argv)) return false;

      if (mdpFileOpt.getCount())
         if (mdpFileOpt.getValue()[0] != "-")
            mdpInput.open(mdpFileOpt.getValue()[0].c_str());
         else
         {
            
            if (debugLevel)
               cout << "Taking input from stdin." << endl;
            mdpInput.copyfmt(std::cin);
            mdpInput.clear(std::cin.rdstate());
            mdpInput.std::basic_ios<char>::rdbuf(std::cin.rdbuf());
         }

      if (navFileOpt.getCount())
         FICOutput.open(navFileOpt.getValue()[0].c_str(), std::ios::out);
      else
         FICOutput.clear(std::ios::badbit);
      
      logActive = false;
      if (logFileOpt.getCount())
      {
         logfp = fopen( logFileOpt.getValue()[0].c_str(), "wt");
         if (logfp!=0) logActive = true;
          else cout << "Log file open failed.  Continuing" << endl;
      }
      obsCount = 0;
      firstObs = true;
      numSubframesCollected = 0;
      paritySuccessCount = 0;
      parityFailCount = 0;
      firstNavSF = true;
      earliestTime = gpstk::CommonTime::BEGINNING_OF_TIME;
      latestTime = gpstk::CommonTime::END_OF_TIME;
      
      debugCount = 0;
      
      return true;
   }
   
protected:
   virtual void spinUp()
   {
      if (!mdpInput)
      {
         cout << "Error: could not open input." << endl;
         exit(-1);
      }

      mdpInput.exceptions(fstream::failbit);

      if (FICOutput)
         FICOutput.exceptions(fstream::failbit);
      
      CommonTime timeNow;
      std::string timeStr = printTime(timeNow,"%02H:%02M, %02m/%02d/%02y");
      fich.header = "Generated by mdp2fic on " + timeStr;

      if (FICOutput)
         FICOutput << fich;
   }

   virtual void process(MDPNavSubframe& nav)
   {
      short sfid = nav.getSFID();
         // For now, only consider SF 1-3 (ephemeris).  
         // Ignore the almanac 
      if (sfid > 3) processSubframes4and5(nav);
       else  processSubframes1to3(nav);
      return;
   }

   void processSubframes4and5(MDPNavSubframe& nav)
   {
         // For now, just look at L1 C/A navigation message
      if (nav.range != rcCA || nav.carrier != ccL1)
         return;
      
         // Pull the time from the subframe
      short week = static_cast<GPSWeekSecond>(nav.time).week;
      long sow = nav.getHOWTime();
      if ( sow >604800)
         return;

      CommonTime howTime(week, sow);

      NavIndex ni(RangeCarrierPair(nav.range, nav.carrier), nav.prn);
      
      AlmMap::iterator a;
      a = almData.find(ni);
      if (a==almData.end())
      {
         UniqueAlmStore init( ni, nav.nav );
         //pair<NavIndex,gpstk::UniqueAlmStore> = node(ni,init);
         almData.insert( make_pair(ni,init) );
         a = almData.find(ni);
         if (a==almData.end())
         {
            cerr << "Almanac map insertion failed in mdp2fic.processSubframes4and5." << endl;
            exit(1);
         }
         cout << "Inserted a new almanac map for PRN " << nav.prn << endl;
      }
      UniqueAlmStore& uas = a->second;
      uas.newSubframe(nav);
      if (uas.readyToWrite()) uas.write( FICOutput );
   }
   
   void processSubframes1to3(MDPNavSubframe& nav)
   {
         // For now, just look at L1 C/A navigation message
      if (nav.range != rcCA || nav.carrier != ccL1)
         return;

      NavIndex ni(RangeCarrierPair(nav.range, nav.carrier), nav.prn);
      ephData[ni] = nav;

      long sfa[10];
      nav.fillArray(sfa);
      uint32_t uint_sfa[10];
 
      for( int j = 0; j < 10; j++ )
         uint_sfa[j] = static_cast<uint32_t>( sfa[j] );

      numSubframesCollected++;
      if (gpstk::EngNav::checkParity(uint_sfa))
      {
         paritySuccessCount++;
         ephPageStore[ni][nav.getSFID()] = nav;
         EngEphemeris engEph;
         if (makeEngEphemeris(engEph, ephPageStore[ni]))
         {
            currentPRN = engEph.getPRNID();     // debug
            if (firstNavSF) 
            {
               earliestTime = engEph.getTransmitTime();
               firstNavSF = false;
            }
            latestTime = engEph.getTransmitTime();
            processEphemeris( engEph, ephPageStore[ni] );
         }
      }
      else parityFailCount++;
   } // end of process(MDPNavSubframe)

   virtual void process()
   {
      MDPHeader header;
      MDPNavSubframe nav;
      MDPObsEpoch obs;

         // Ought to be able to catch EOF here....
      try
      {
         mdpInput >> header;
         switch (header.id)
         {
            case MDPNavSubframe::myId :
               mdpInput >> nav;
               process(nav);
               break;
            
            case MDPObsEpoch::myId :
               mdpInput >> obs;
               obsCount++;
               if (debugLevel && (obsCount % 1000)==0) cout << "obsCount: " << obsCount << endl;
               break;
         }
      }
      catch (gpstk::Exception &exc)
      { 
         cout << "Caught a GPSTk Exception in process()." << endl;
         cout << exc << endl; 
         timeToDie = true;
         return;
      }  
      catch (std::exception &exc)
      {
         cout << "Trapped an exception in process()." << endl;
         timeToDie = true;
         return;
      }
      catch (...)
      {
         cout << "I don't know HOW we got here, but we caught an unexpcted exception." << endl;
         timeToDie = true;
         return;
      }
      timeToDie = !mdpInput;
   }

   virtual void shutDown()
   {
      cout << "Entering shutDown()." << endl;
      writeLogFile( );
   }
   
   void writeLogFile( )
   {
      typedef PrnBELogMap::const_iterator ciPRN;
      if (logActive)
      {
         std::string timestring = "%02m/%02d/%02y %03j %02H:%02M:%02S, GPS Week %F, SOW %6.0g";
         fprintf(logfp,"Output log from mdp2fic.\n");
         fprintf(logfp,"Earliest Transmit Time: %s\n",printTime(earliestTime,timestring).c_str());
         fprintf(logfp,"Latest Transmit Time  : %s\n",printTime(latestTime,timestring).c_str());
         fprintf(logfp,"Statistics on parity checks\n");
         fprintf(logfp,"Total number of subframes processed: %7ld\n",numSubframesCollected);
         fprintf(logfp,"Number of successful parity checks : %7ld\n",paritySuccessCount);
         fprintf(logfp,"Number of failed parity chekcs     : %7ld\n",parityFailCount);
         double perCentFail = (parityFailCount *100.0) / numSubframesCollected;
         fprintf(logfp,"Percent of subframes failing parity: %7.2lf\n",perCentFail);
 
         ciPRN pp;
         for (pp=prnBEmap.begin();pp!=prnBEmap.end();++pp)
         {
            int prnID = pp->first;
            const BELogMap& blm = pp->second;
            int numEntries = blm.size();
            typedef BELogMap::const_iterator ciBLM;
            fprintf(logfp,"\nSummary of Broadcast Ephemerides for PRN %02d\n",prnID);
            fprintf(logfp,"%d unique ephemerides found.\n",numEntries);
            fprintf(logfp,"%s\n",BELogEntry::header.c_str());
            
               // NOTE: The table is stored in the wrong order for output.
               // I had to use the Toe in the key for uniqueness, however,
               // I want the ending table ordered by earliest HOW.  HOW is 
               // in the object, so now that we have a unique list, it can
               // be used to re-order a new map
            std::map<double,BELogEntry> reorder;
            for (ciBLM bp=blm.begin();bp!=blm.end();++bp)
            {
               const BELogEntry& ble = bp->second;
               double HOW = static_cast<GPSWeekSecond>(ble.getHOW()).sow;
               pair<double,BELogEntry> node(HOW,ble);
               reorder.insert(node);
            }
            typedef std::map<double,BELogEntry>::const_iterator rei;
            for (rei rp=reorder.begin();rp!=reorder.end();++rp)
            {
               const BELogEntry& bler = rp->second;
               fprintf(logfp,"%s\n", bler.getStr().c_str() );
            }
         }
         fclose(logfp);
      }
   }

   void processEphemeris( gpstk::EngEphemeris engEph, 
                          gpstk::EphemerisPages ephPages )
   {

         // Construct a BELogEntry and see if it already exists in the 
         // map for the PRN.  If it does, we already stored this one, 
         // move on.  If not, convert the information to Block 109 and 
         // Block 9 and write it out.               
      BELogEntry curBELog( engEph );
      unsigned long key = curBELog.getKey();
      pair<long,BELogEntry> qnode(key,curBELog);

      bool needToOutput = false;
      PrnBELogMap::iterator pmap = prnBEmap.find( engEph.getPRNID() );
            
          // May need to add this PRN to the map.
      if (pmap==prnBEmap.end())
      {
         pair<long,BELogEntry> qnode(key,curBELog);
         BELogMap blm;
         blm.insert(qnode);
         pair<int,BELogMap> pnode( (int) engEph.getPRNID(), blm);
         prnBEmap.insert( pnode );
         needToOutput = true;
      }
      else
      {
         BELogMap& blmr = pmap->second;
         BELogMap::iterator iBLM = blmr.find( key );
         if (iBLM==blmr.end())
         {
            pair<long,BELogEntry> qnode(key,curBELog);
            blmr.insert(qnode);
            needToOutput = true;
         }
         else
         {
            BELogEntry& ble = iBLM->second;
            ble.increment();
         }
      }
      
      if (needToOutput)
      {
         EphemerisPages::const_iterator MDPsf[4];
         MDPsf[1] = ephPages.find(1);
         MDPsf[2] = ephPages.find(2);
         MDPsf[3] = ephPages.find(3);
         FICData109 new109( engEph.getPRNID(),
                            MDPsf[1]->second.subframe,
                            MDPsf[2]->second.subframe,
                            MDPsf[3]->second.subframe );
         FICData9   new9( new109, engEph );
         FICOutput << new109;
         FICOutput << new9;
      }
   }
   
private:
   gpstk::FICHeader fich;
   MDPStream mdpInput;
   FICStream FICOutput;
   MDPEpoch epoch;
   
   long obsCount;
   
      // Defs and maps related to ephemeris handling
   //typedef std::pair<gpstk::RangeCode, gpstk::CarrierCode> RangeCarrierPair;
   //typedef std::pair<RangeCarrierPair, short> NavIndex;
   typedef std::map<NavIndex, gpstk::MDPNavSubframe> NavMap;
   NavMap ephData;
   std::map<NavIndex, gpstk::EphemerisPages> ephPageStore;
   std::map<NavIndex, gpstk::EngEphemeris> ephStore;
   
      // Ordered list of BELogEntries   
   typedef std::map<long,BELogEntry> BELogMap;
   
      // For each PRN, there is a map pointing to the BE logs for that SV
   typedef std::map<int, BELogMap> PrnBELogMap;
   PrnBELogMap prnBEmap;

      // Def and maps related to almanac handling
   typedef std::map<NavIndex, gpstk::UniqueAlmStore> AlmMap;
   AlmMap almData;

      // Output file
   FILE *logfp;
   bool logActive;
   
      //debug
   int currentPRN;
   int debugCount;
   
   long numSubframesCollected;
   long paritySuccessCount;
   long parityFailCount;
   bool firstNavSF;
   gpstk::CommonTime earliestTime;
   gpstk::CommonTime latestTime;
   
   bool firstObs;
   gpstk::CommonTime prevTime;
   gpstk::CommandOptionWithAnyArg mdpFileOpt, navFileOpt, logFileOpt;
};


int main(int argc, char *argv[])
{
   try
   {
      MDP2FIC crap(argv[0]);
      if (!crap.initialize(argc, argv))
         exit(0);

      crap.run();
   }
   catch (gpstk::Exception &exc)
   { cout << exc << endl; }
   catch (std::exception &exc)
   { cout << "Caught std::exception " << exc.what() << endl;  }
   catch (...)
   { cout << "Caught unknown exception" << endl; }
}
