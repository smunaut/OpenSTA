// OpenSTA, Static Timing Analyzer
// Copyright (c) 2022, Parallax Software, Inc.
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "MakeTimingModel.hh"

#include <map>

#include "Debug.hh"
#include "Units.hh"
#include "Transition.hh"
#include "Liberty.hh"
#include "TimingArc.hh"
#include "TableModel.hh"
#include "liberty/LibertyBuilder.hh"
#include "Network.hh"
#include "PortDirection.hh"
#include "Corner.hh"
#include "DcalcAnalysisPt.hh"
#include "dcalc/GraphDelayCalc1.hh"
#include "Sdc.hh"
#include "StaState.hh"
#include "Graph.hh"
#include "PathEnd.hh"
#include "Search.hh"
#include "Sta.hh"
#include "VisitPathEnds.hh"

namespace sta {

MakeTimingModel::MakeTimingModel(const Corner *corner,
                                 Sta *sta) :
  StaState(sta),
  sta_(sta),
  corner_(corner),
  min_max_(MinMax::max()),
  lib_builder_(new LibertyBuilder)
{
}

MakeTimingModel::~MakeTimingModel()
{
  delete lib_builder_;
}

LibertyLibrary *
MakeTimingModel::makeTimingModel(const char *cell_name,
                                 const char *filename)
{
  makeLibrary(cell_name, filename);
  makeCell(cell_name, filename);
  makePorts();

  for (Clock *clk : *sdc_->clocks())
    sta_->setPropagatedClock(clk);

  sta_->searchPreamble();
  graph_ = sta_->graph();

  findTimingFromInputs();
  findClkedOutputPaths();

  cell_->finish(false, report_, debug_);
  return library_;
}

void
MakeTimingModel::makeLibrary(const char *cell_name,
                             const char *filename)
{
  library_ = network_->makeLibertyLibrary(cell_name, filename);
  LibertyLibrary *default_lib = network_->defaultLibertyLibrary();
  *library_->units()->timeUnit() = *default_lib->units()->timeUnit();
  *library_->units()->capacitanceUnit() = *default_lib->units()->capacitanceUnit();
  *library_->units()->voltageUnit() = *default_lib->units()->voltageUnit();
  *library_->units()->resistanceUnit() = *default_lib->units()->resistanceUnit();
  *library_->units()->pullingResistanceUnit() = *default_lib->units()->pullingResistanceUnit();
  *library_->units()->powerUnit() = *default_lib->units()->powerUnit();
  *library_->units()->distanceUnit() = *default_lib->units()->distanceUnit();

  for (RiseFall *rf : RiseFall::range()) {
    library_->setInputThreshold(rf, default_lib->inputThreshold(rf));
    library_->setOutputThreshold(rf, default_lib->outputThreshold(rf));
    library_->setSlewLowerThreshold(rf, default_lib->slewLowerThreshold(rf));
    library_->setSlewUpperThreshold(rf, default_lib->slewUpperThreshold(rf));
  }

  library_->setDelayModelType(default_lib->delayModelType());
  library_->setNominalProcess(default_lib->nominalProcess());
  library_->setNominalVoltage(default_lib->nominalVoltage());
  library_->setNominalTemperature(default_lib->nominalTemperature());
}

void
MakeTimingModel::makeCell(const char *cell_name,
                          const char *filename)
{
  cell_ = lib_builder_->makeCell(library_, cell_name, filename);
}

void
MakeTimingModel::makePorts()
{
  const DcalcAnalysisPt *dcalc_ap = corner_->findDcalcAnalysisPt(min_max_);
  Instance *top_inst = network_->topInstance();
  Cell *top_cell = network_->cell(top_inst);
  CellPortIterator *port_iter = network_->portIterator(top_cell);
  while (port_iter->hasNext()) {
    Port *port = port_iter->next();
    const char *port_name = network_->name(port);
    if (network_->isBus(port)) {
      int from_index = network_->fromIndex(port);
      int to_index = network_->toIndex(port);
      BusDcl *bus_dcl = new BusDcl(port_name, from_index, to_index);
      library_->addBusDcl(bus_dcl);
      LibertyPort *lib_port = lib_builder_->makeBusPort(cell_, port_name,
                                                        from_index, to_index,
                                                        bus_dcl);
      lib_port->setDirection(network_->direction(port));
      PortMemberIterator *member_iter = network_->memberIterator(port);
      while (member_iter->hasNext()) {
        Port *bit_port = member_iter->next();
        Pin *pin = network_->findPin(top_inst, bit_port);
        LibertyPort *lib_bit_port = modelPort(pin);
        float load_cap = graph_delay_calc_->loadCap(pin, dcalc_ap);
        lib_bit_port->setCapacitance(load_cap);
      }
    }
    else {
      LibertyPort *lib_port = lib_builder_->makePort(cell_, port_name);
      lib_port->setDirection(network_->direction(port));
      Pin *pin = network_->findPin(top_inst, port);
      float load_cap = graph_delay_calc_->loadCap(pin, dcalc_ap);
      lib_port->setCapacitance(load_cap);
    }
  }
  delete port_iter;
}

////////////////////////////////////////////////////////////////

class MakeEndTimingArcs : public PathEndVisitor
{
public:
  MakeEndTimingArcs(Sta *sta);
  MakeEndTimingArcs(const MakeEndTimingArcs&) = default;
  virtual ~MakeEndTimingArcs() {}
  virtual PathEndVisitor *copy() const;
  virtual void visit(PathEnd *path_end);
  void setInputPin(const Pin *input_pin);
  void setInputRf(const RiseFall *input_rf);
  const ClockMargins &margins() const { return margins_; }

private:
  Sta *sta_;
  const Pin *input_pin_;
  const RiseFall *input_rf_;
  ClockMargins margins_;
};

MakeEndTimingArcs::MakeEndTimingArcs(Sta *sta) :
  sta_(sta)
{
}

PathEndVisitor *
MakeEndTimingArcs::copy() const
{
  return new MakeEndTimingArcs(*this);
}

void
MakeEndTimingArcs::setInputPin(const Pin *input_pin)
{
  input_pin_ = input_pin;
  margins_.clear();
}

void
MakeEndTimingArcs::setInputRf(const RiseFall *input_rf)
{
  input_rf_ = input_rf;
}

void
MakeEndTimingArcs::visit(PathEnd *path_end)
{
  ClockEdge *tgt_clk_edge = path_end->targetClkEdge(sta_);
  Debug *debug = sta_->debug();
  const MinMax *min_max = path_end->minMax(sta_);
  debugPrint(debug, "make_timing_model", 2, "%s %s -> clock %s %s %s",
             sta_->network()->pathName(input_pin_),
             input_rf_->shortName(),
             tgt_clk_edge->name(),
             path_end->typeName(),
             min_max->asString());
  if (debug->check("make_timing_model", 3))
    sta_->reportPathEnd(path_end);
  Arrival data_arrival = path_end->path()->arrival(sta_);
  Delay clk_latency = path_end->targetClkDelay(sta_);
  ArcDelay check_setup = path_end->margin(sta_);
  float margin = data_arrival - clk_latency + check_setup;
  RiseFallMinMax &margins = margins_[tgt_clk_edge];
  margins.setValue(input_rf_, min_max, margin);
}

// input -> register setup/hold
// input -> output combinational paths
// Use default input arrival (set_input_delay with no clock) from inputs
// to find downstream register checks and output ports.
void
MakeTimingModel::findTimingFromInputs()
{
  VisitPathEnds visit_ends(sta_);
  MakeEndTimingArcs end_visitor(sta_);
  InstancePinIterator *input_iter = network_->pinIterator(network_->topInstance());
  while (input_iter->hasNext()) {
    Pin *input_pin = input_iter->next();
    if (network_->direction(input_pin)->isInput()
        && !sta_->isClockSrc(input_pin)) {
      end_visitor.setInputPin(input_pin);
      OutputPinDelays output_delays;
      for (RiseFall *input_rf : RiseFall::range()) {
        RiseFallBoth *input_rf1 = input_rf->asRiseFallBoth();
        sta_->setInputDelay(input_pin, input_rf1,
                            sdc_->defaultArrivalClock(),
                            sdc_->defaultArrivalClockEdge()->transition(),
                            nullptr, false, false, MinMaxAll::all(), false, 0.0);

        PinSet *from_pins = new PinSet;
        from_pins->insert(input_pin);
        ExceptionFrom *from = sta_->makeExceptionFrom(from_pins, nullptr, nullptr,
                                                      input_rf1);
        search_->deleteFilteredArrivals();
        search_->findFilteredArrivals(from, nullptr, nullptr, false);

        end_visitor.setInputRf(input_rf);
        for (Vertex *end : *search_->endpoints())
          visit_ends.visitPathEnds(end, corner_, MinMaxAll::all(), true, &end_visitor);
        findOutputDelays(input_rf, output_delays);

        sta_->removeInputDelay(input_pin, input_rf1,
                               sdc_->defaultArrivalClock(),
                               sdc_->defaultArrivalClockEdge()->transition(),
                               MinMaxAll::all());
      }
      makeSetupHoldTimingArcs(input_pin, end_visitor.margins());
      makeInputOutputTimingArcs(input_pin, output_delays);
    }
  }
}

void
MakeTimingModel::findOutputDelays(const RiseFall *input_rf,
                                  OutputPinDelays &output_pin_delays)
{
  InstancePinIterator *output_iter = network_->pinIterator(network_->topInstance());
  while (output_iter->hasNext()) {
    Pin *output_pin = output_iter->next();
    if (network_->direction(output_pin)->isOutput()) {
      Vertex *output_vertex = graph_->pinLoadVertex(output_pin);
      VertexPathIterator path_iter(output_vertex, this);
      while (path_iter.hasNext()) {
        PathVertex *path = path_iter.next();
        if (search_->matchesFilter(path, nullptr)) {
          const RiseFall *output_rf = path->transition(sta_);
          const MinMax *min_max = path->minMax(sta_);
          Arrival delay = path->arrival(sta_);
          OutputDelays &delays = output_pin_delays[output_pin];
          delays.delays.mergeValue(output_rf, min_max, delay);
          delays.rf_path_exists[input_rf->index()][output_rf->index()] = true;
        }
      }
    }
  }
}

void
MakeTimingModel::makeSetupHoldTimingArcs(const Pin *input_pin,
                                         const ClockMargins &clk_margins)
{
  for (auto clk_edge_margins : clk_margins) {
    ClockEdge *clk_edge = clk_edge_margins.first;
    RiseFallMinMax &margins = clk_edge_margins.second;
    for (MinMax *min_max : MinMax::range()) {
      bool setup = (min_max == MinMax::max());
      TimingArcAttrs *attrs = nullptr;
      for (RiseFall *input_rf : RiseFall::range()) {
        float margin;
        bool exists;
        margins.value(input_rf, min_max, margin, exists);
        if (exists) {
          debugPrint(debug_, "make_timing_model", 2, "%s %s %s -> clock %s %s",
                     sta_->network()->pathName(input_pin),
                     input_rf->shortName(),
                     min_max == MinMax::max() ? "setup" : "hold",
                     clk_edge->name(),
                     delayAsString(margin, sta_));
          ScaleFactorType scale_type = setup
            ? ScaleFactorType::setup
            : ScaleFactorType::hold;
          TimingModel *check_model = makeScalarCheckModel(margin, scale_type, input_rf);
          if (attrs == nullptr)
            attrs = new TimingArcAttrs();
          attrs->setModel(input_rf, check_model);
        }
      }
      if (attrs) {
        LibertyPort *input_port = modelPort(input_pin);
        for (const Pin *clk_pin : clk_edge->clock()->pins()) {
          LibertyPort *clk_port = modelPort(clk_pin);
          RiseFall *clk_rf = clk_edge->transition();
          TimingRole *role = setup ? TimingRole::setup() : TimingRole::hold();
          lib_builder_->makeFromTransitionArcs(cell_, clk_port,
                                               input_port, nullptr,
                                               clk_rf, role, attrs);
        }
      }
    }
  }
}

void
MakeTimingModel::makeInputOutputTimingArcs(const Pin *input_pin,
                                           OutputPinDelays &output_pin_delays)
{
  const DcalcAnalysisPt *dcalc_ap = corner_->findDcalcAnalysisPt(min_max_);
  for (auto out_pin_delay : output_pin_delays) {
    const Pin *output_pin = out_pin_delay.first;
    OutputDelays &output_delays = out_pin_delay.second;
    TimingArcAttrs *attrs = nullptr;
    for (RiseFall *output_rf : RiseFall::range()) {
      MinMax *min_max = MinMax::max();
      float delay;
      bool exists;
      output_delays.delays.value(output_rf, min_max, delay, exists);
      if (exists) {
        debugPrint(debug_, "make_timing_model", 2, "%s -> %s %s delay %s",
                   network_->pathName(input_pin),
                   network_->pathName(output_pin),
                   output_rf->shortName(),
                   delayAsString(delay, sta_));
        Vertex *output_vertex = graph_->pinLoadVertex(output_pin);
        Slew slew = graph_->slew(output_vertex, output_rf, dcalc_ap->index());
        TimingModel *gate_model = makeScalarGateModel(delay, slew, output_rf);
        if (attrs == nullptr)
          attrs = new TimingArcAttrs();
        attrs->setModel(output_rf, gate_model);
      }
    }
    if (attrs) {
      LibertyPort *output_port = modelPort(output_pin);
      LibertyPort *input_port = modelPort(input_pin);
      attrs->setTimingSense(output_delays.timingSense());
      lib_builder_->makeCombinationalArcs(cell_, input_port,
                                          output_port, nullptr,
                                          true, true, attrs);
    }
  }
}

////////////////////////////////////////////////////////////////

// Rewrite to use non-filtered arrivals at outputs from each clock.
void
MakeTimingModel::findClkedOutputPaths()
{
  InstancePinIterator *output_iter = network_->pinIterator(network_->topInstance());
  while (output_iter->hasNext()) {
    Pin *output_pin = output_iter->next();    
    if (network_->direction(output_pin)->isOutput()) {
      LibertyPort *output_port = modelPort(output_pin);
      for (Clock *clk : *sdc_->clocks()) {
        for (const Pin *clk_pin : clk->pins()) {
          LibertyPort *clk_port = modelPort(clk_pin);
          for (RiseFall *clk_rf : RiseFall::range()) {
            TimingArcAttrs *attrs = nullptr;
            for (RiseFall *output_rf : RiseFall::range()) {
              RiseFallBoth *output_rf1 = output_rf->asRiseFallBoth();
              MinMax *min_max = MinMax::max();
              MinMaxAll *min_max1 = min_max->asMinMaxAll();
              sta_->setOutputDelay(output_pin, output_rf1, clk, clk_rf,
                                   nullptr, false, false, min_max1, false, 0.0);
        
              ClockSet *from_clks = new ClockSet;
              from_clks->insert(clk);
              ExceptionFrom *from = sta_->makeExceptionFrom(nullptr, from_clks, nullptr,
                                                            clk_rf->asRiseFallBoth());
              PinSet *to_pins = new PinSet;
              to_pins->insert(output_pin);
              ExceptionTo *to = sta_->makeExceptionTo(to_pins, nullptr, nullptr,
                                                      output_rf1, output_rf1);

              PathEndSeq *ends = sta_->findPathEnds(from, nullptr, to, false, corner_, min_max1,
                                                    1, 1, false, -INF, INF, false, nullptr,
                                                    true, false, false, false, false, false);
              if (!ends->empty()) {
                debugPrint(debug_, "make_timing_model", 1, "clock %s -> output %s",
                           clk->name(),
                           network_->pathName(output_pin));
                PathEnd *end = (*ends)[0];
                if (debug_->check("make_timing_model", 3))
                  sta_->reportPathEnd(end);
                Arrival delay = end->path()->arrival(sta_);
                Slew slew = end->path()->slew(sta_);
                TimingModel *gate_model = makeScalarGateModel(delay, slew, output_rf);
                if (attrs == nullptr)
                  attrs = new TimingArcAttrs();
                attrs->setModel(output_rf, gate_model);
              }
              sta_->removeOutputDelay(output_pin, output_rf1, clk, clk_rf, MinMaxAll::max());
            }
            if (attrs)
              lib_builder_->makeFromTransitionArcs(cell_, clk_port,
                                                   output_port, nullptr,
                                                   clk_rf, TimingRole::regClkToQ(),
                                                   attrs);
          }
        }
      }
    }
  }
}

LibertyPort *
MakeTimingModel::modelPort(const Pin *pin)
{
  return cell_->findLibertyPort(network_->name(network_->port(pin)));
}

TimingModel *
MakeTimingModel::makeScalarCheckModel(float value,
                                      ScaleFactorType scale_factor_type,
                                      RiseFall *rf)
{
  Table *table = new Table0(value);
  TableTemplate *tbl_template =
    library_->findTableTemplate("scalar", TableTemplateType::delay);
  TableModel *table_model = new TableModel(table, tbl_template,
                                           scale_factor_type, rf);
  CheckTableModel *check_model = new CheckTableModel(table_model, nullptr);
  return check_model;
}

TimingModel *
MakeTimingModel::makeScalarGateModel(Delay delay,
                                     Slew slew,
                                     RiseFall *rf)
{
  Table *delay_table = new Table0(delay);
  Table *slew_table = new Table0(slew);
  TableTemplate *tbl_template =
    library_->findTableTemplate("scalar", TableTemplateType::delay);
  TableModel *delay_model = new TableModel(delay_table, tbl_template,
                                           ScaleFactorType::cell, rf);
  TableModel *slew_model = new TableModel(slew_table, tbl_template,
                                          ScaleFactorType::cell, rf);
  GateTableModel *gate_model = new GateTableModel(delay_model, nullptr,
                                                  slew_model, nullptr);
  return gate_model;
}

OutputDelays::OutputDelays()
{
  rf_path_exists[RiseFall::riseIndex()][RiseFall::riseIndex()] = false;
  rf_path_exists[RiseFall::riseIndex()][RiseFall::fallIndex()] = false;
  rf_path_exists[RiseFall::fallIndex()][RiseFall::riseIndex()] = false;
  rf_path_exists[RiseFall::fallIndex()][RiseFall::fallIndex()] = false;
}

TimingSense
OutputDelays::timingSense() const
{
  if (rf_path_exists[RiseFall::riseIndex()][RiseFall::riseIndex()]
      && rf_path_exists[RiseFall::riseIndex()][RiseFall::fallIndex()]
      && rf_path_exists[RiseFall::fallIndex()][RiseFall::riseIndex()]
      && rf_path_exists[RiseFall::fallIndex()][RiseFall::fallIndex()])
    return TimingSense::non_unate;
  else if (rf_path_exists[RiseFall::riseIndex()][RiseFall::riseIndex()]
           && rf_path_exists[RiseFall::fallIndex()][RiseFall::fallIndex()]
           && !rf_path_exists[RiseFall::riseIndex()][RiseFall::fallIndex()]
           && !rf_path_exists[RiseFall::fallIndex()][RiseFall::riseIndex()])
    return TimingSense::positive_unate;
  else if (rf_path_exists[RiseFall::riseIndex()][RiseFall::fallIndex()]
           && rf_path_exists[RiseFall::fallIndex()][RiseFall::riseIndex()]
           && !rf_path_exists[RiseFall::riseIndex()][RiseFall::riseIndex()]
           && !rf_path_exists[RiseFall::fallIndex()][RiseFall::fallIndex()])
    return TimingSense::negative_unate;
  else if (rf_path_exists[RiseFall::riseIndex()][RiseFall::riseIndex()]
           || rf_path_exists[RiseFall::riseIndex()][RiseFall::fallIndex()]
           || rf_path_exists[RiseFall::fallIndex()][RiseFall::riseIndex()]
           || rf_path_exists[RiseFall::fallIndex()][RiseFall::fallIndex()])
    return TimingSense::non_unate;
  else
    return TimingSense::none;
}

} // namespace