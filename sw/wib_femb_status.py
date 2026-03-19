import argparse

from wib import WIB
import wib_pb2 as wibpb

# Example usage: python wib_femb_status.py -w {wib_address}
# Prints out current power status of FEMBs for the WIB
# Pass in --voltages flag to get detailed voltage/current readings from WIB DCDC modules for the FEMBs
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Check power status of FEMBs on a WIB')
    parser.add_argument('--wib_server','-w',default='127.0.0.1',help='IP of wib_server to connect to [127.0.0.1]')
    parser.add_argument('--voltages',action='store_true',help='Retrive voltage/current readings for FEMBs')

    args = parser.parse_args()
    wib = WIB(args.wib_server)

    req = wibpb.GetFEMBStatus()
    req.read_voltages = args.voltages
    rep = wibpb.GetFEMBStatus.FEMBStatus()
    wib.send_command(req, rep)
    wib.print_femb_status(rep)
